// Inclusión de bibliotecas estándar y de ESP-IDF
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "lwip/apps/sntp.h"
#include "i2c_bus.h"
#include "driver/rmt.h"
#include "led_strip.h"
#include "touch.h"
#include "audio.h"
#include "es8311.h"
#include "board.h"

#include <esp_http_server.h>
#include <nvs_flash.h>
#include <mqtt_client.h>
#include "cJSON.h"

// Definición de macros para el código
#define MIN(a, b) (((a) < (b)) ? (a) : (b))  // Macro para obtener el valor mínimo entre a y b

// Definición de configuraciones para el punto de acceso (AP) y la estación (STA) de WiFi
#define AP_WIFI_SSID      "MyESP32AP-MNF"  // SSID del punto de acceso WiFi
#define AP_WIFI_PASS      "password123"    // Contraseña del punto de acceso WiFi
#define AP_WIFI_CHANNEL   1                // Canal del punto de acceso WiFi
#define AP_MAX_STA_CONN   4                // Número máximo de conexiones de estación permitidas

#define STA_WIFI_SSID     "YourSSID"       // SSID de la red WiFi a la que se conectará la estación
#define STA_WIFI_PASS     "YourPassword"   // Contraseña de la red WiFi a la que se conectará la estación

static const char *TAG = "main";  // Etiqueta de registro para esta aplicación
static const char *MQTT_TAG = "MQTT_CLIENT";  // Etiqueta de registro para el cliente MQTT

static EventGroupHandle_t wifi_event_group;  // Grupo de eventos para WiFi
const int WIFI_CONNECTED_BIT = BIT0;  // Indicador de conexión WiFi

uint8_t mac[16];  // Array para almacenar la dirección MAC
led_strip_t *strip;  // Puntero a la tira de LEDs
static esp_mqtt_client_handle_t mqtt_client;  // Manejador del cliente MQTT

// Función que maneja las solicitudes GET a la ruta raíz (/)
esp_err_t index_get_handler(httpd_req_t *req) {
    extern const uint8_t index_html_start[] asm("_binary_index_html_start");
    extern const uint8_t index_html_end[] asm("_binary_index_html_end");
    const size_t index_html_size = (index_html_end - index_html_start);  // Tamaño del archivo index.html
    httpd_resp_send(req, (const char *)index_html_start, index_html_size);  // Envía el archivo index.html como respuesta
    return ESP_OK;
}

// Función que maneja las solicitudes POST a la ruta /command
esp_err_t command_post_handler(httpd_req_t *req) {
    char buf[100];  // Buffer para almacenar los datos de la solicitud
    int ret, remaining = req->content_len;  // Variables para manejar la longitud del contenido

    // Lee el contenido de la solicitud
    while (remaining > 0) {
        if ((ret = httpd_req_recv(req, buf, MIN(remaining, sizeof(buf)))) <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;  // Reintenta si hay un error de tiempo de espera
            }
            return ESP_FAIL;  // Devuelve un error si no se puede recibir la solicitud
        }
        remaining -= ret;  // Actualiza el tamaño restante del contenido
    }
    buf[req->content_len] = '\0';  // Asegura que el buffer sea una cadena de caracteres

    ESP_LOGI(TAG, "Received command: %s", buf);  // Registra el comando recibido

    // Parsear el JSON recibido
    cJSON *json = cJSON_Parse(buf);
    if (json == NULL) {
        ESP_LOGE(TAG, "Error parsing JSON");  // Error al parsear JSON
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");  // Responde con un error 400
        return ESP_FAIL;
    }

    // Obtener el comando del JSON
    cJSON *command = cJSON_GetObjectItem(json, "command");
    if (cJSON_IsString(command) && (command->valuestring != NULL)) {
        ESP_LOGI(TAG, "Command received: %s", command->valuestring);  // Registra el comando recibido
        // Publicar el comando al broker MQTT
        if (mqtt_client != NULL) {
            esp_mqtt_client_publish(mqtt_client, "test", command->valuestring, 0, 1, 0);  // Publicar al tópico "test"
            ESP_LOGI(TAG, "Published command to MQTT: %s", command->valuestring);  // Log de publicación
        } else {
            ESP_LOGE(TAG, "MQTT client not initialized");  // Error si el cliente MQTT no está inicializado
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "MQTT client not initialized");  // Responde con un error 500
            cJSON_Delete(json);  // Libera la memoria del JSON
            return ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "Invalid command format");  // Error si el formato del comando no es válido
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid command format");  // Responde con un error 400
        cJSON_Delete(json);  // Libera la memoria del JSON
        return ESP_FAIL;
    }

    cJSON_Delete(json);  // Libera la memoria del JSON
    httpd_resp_sendstr(req, "Command received");  // Envía una respuesta al cliente
    return ESP_OK;
}

// Estructura URI para la ruta raíz (/)
httpd_uri_t uri_index = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = index_get_handler,  // Maneja las solicitudes GET a la ruta raíz
    .user_ctx  = NULL
};

// Estructura URI para la ruta /command
httpd_uri_t uri_command = {
    .uri       = "/command",
    .method    = HTTP_POST,
    .handler   = command_post_handler,  // Maneja las solicitudes POST a la ruta /command
    .user_ctx  = NULL
};

// Función para iniciar el servidor web
httpd_handle_t start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();  // Configuración por defecto del servidor HTTP
    httpd_handle_t server = NULL;

    // Inicia el servidor web y registra los handlers de las URIs
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &uri_index);  // Registra el handler para la ruta raíz (/)
        httpd_register_uri_handler(server, &uri_command);  // Registra el handler para la ruta /command
    } else {
        ESP_LOGI(TAG, "Failed to start server!");  // Log si no se puede iniciar el servidor
    }
    return server;
}

// Callback para manejar eventos del cliente MQTT
static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event) {
    esp_mqtt_client_handle_t client = event->client;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(MQTT_TAG, "MQTT_EVENT_CONNECTED");  // Log cuando el cliente MQTT está conectado
            esp_mqtt_client_subscribe(client, "test", 1);  // Se suscribe al tópico "test"
            esp_mqtt_client_publish(client, "test", "mensaje de prueba desde ESP32", 0, 1, 0);  // Publica un mensaje de prueba
            mqtt_client = client;  // Asigna el cliente MQTT a la variable global
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(MQTT_TAG, "MQTT_EVENT_DISCONNECTED");  // Log cuando el cliente MQTT está desconectado
            mqtt_client = NULL;  // Limpia el cliente MQTT global
            break;
        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(MQTT_TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);  // Log cuando se suscribe a un tópico
            break;
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(MQTT_TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);  // Log cuando se des-suscribe de un tópico
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(MQTT_TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);  // Log cuando se publica un mensaje
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(MQTT_TAG, "MQTT_EVENT_DATA");  // Log cuando se recibe datos del tópico
            printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);  // Imprime el tópico del mensaje
            printf("DATA=%.*s\r\n", event->data_len, event->data);  // Imprime el contenido del mensaje

            // Añadir código para controlar el LED basado en los datos recibidos del broker
            if (strncmp(event->data, "play", event->data_len) == 0) {
                ESP_LOGI(MQTT_TAG, "Received 'play' command");  // Log cuando se recibe el comando "play"
                // Encender el LED en color verde para indicar el comando "play"
                if (strip != NULL) {
                    ESP_LOGI(MQTT_TAG, "Setting LED to green");  // Log de configuración del color del LED
                    strip->set_pixel(strip, 0, 0, 255, 0);  // Establece el color del LED a verde
                    strip->refresh(strip, 100);  // Actualiza la tira de LEDs
                } else {
                    ESP_LOGE(MQTT_TAG, "LED strip not initialized");  // Error si la tira de LEDs no está inicializada
                }
            }
            // Log adicional para depuración
            ESP_LOGI(MQTT_TAG, "MQTT data received: %.*s", event->data_len, event->data);
            ESP_LOGI(MQTT_TAG, "MQTT data processed");
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGI(MQTT_TAG, "MQTT_EVENT_ERROR");  // Log de error en el cliente MQTT
            break;
        default:
            ESP_LOGI(MQTT_TAG, "Other event id:%d", event->event_id);  // Log para otros eventos MQTT
            break;
    }
    return ESP_OK;
}

// Función para manejar eventos del cliente MQTT
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    ESP_LOGD(MQTT_TAG, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
    mqtt_event_handler_cb(event_data);  // Llama al callback del manejador de eventos MQTT
}

// Función para manejar eventos de WiFi
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "Station connected: " MACSTR, MAC2STR(event->mac));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "Station disconnected: " MACSTR, MAC2STR(event->mac));
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// Función para inicializar el modo punto de acceso (AP) de WiFi
void wifi_init_softap(void) {
    wifi_event_group = xEventGroupCreate();  // Crea un grupo de eventos para WiFi

    ESP_ERROR_CHECK(esp_netif_init());  // Inicializa el netif
    ESP_ERROR_CHECK(esp_event_loop_create_default());  // Crea el bucle de eventos por defecto
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();  // Configuración por defecto de WiFi
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));  // Inicializa el controlador WiFi

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));  // Registra el handler de eventos WiFi

    // Configuración del punto de acceso WiFi
    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "MyESP32AP-MNF",  // SSID del punto de acceso
            .ssid_len = strlen("MyESP32AP-MNF"),  // Longitud del SSID
            .password = "password123",  // Contraseña del punto de acceso
            .max_connection = 4,  // Número máximo de conexiones permitidas
            .authmode = WIFI_AUTH_WPA_WPA2_PSK  // Modo de autenticación
        },
    };
    if (strlen("password123") == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;  // Si no hay contraseña, establece el modo de autenticación como abierto
    }

    // Configuración de la dirección IP estática del punto de acceso
    esp_netif_ip_info_t ip_info;
    ip_info.ip.addr = esp_ip4addr_aton("192.168.4.1");
    ip_info.gw.addr = esp_ip4addr_aton("192.168.4.1");
    ip_info.netmask.addr = esp_ip4addr_aton("255.255.255.0");

    esp_netif_t *netif = esp_netif_create_default_wifi_ap();  // Crea la interfaz de red para el AP
    esp_netif_dhcps_stop(netif);  // Detiene el servidor DHCP para configurar IP estática
    esp_netif_set_ip_info(netif, &ip_info);  // Configura la información de IP estática
    esp_netif_dhcps_start(netif);  // Inicia el servidor DHCP

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));  // Establece el modo del dispositivo a AP
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));  // Configura el punto de acceso WiFi
    ESP_ERROR_CHECK(esp_wifi_start());  // Inicia el controlador WiFi

    ESP_LOGI(TAG, "wifi_init_softap finished. SSID:%s password:%s", "MyESP32AP-MNF", "password123");  // Log que indica que el AP está en funcionamiento
}

// Función para iniciar el cliente MQTT
void mqtt_app_start(void) {
    esp_mqtt_client_config_t mqtt_cfg = {
        .uri = "mqtt://192.168.4.2:1883",  // URI del broker MQTT
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);  // Inicializa el cliente MQTT
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client);  // Registra el handler de eventos MQTT
    esp_mqtt_client_start(client);  // Inicia el cliente MQTT
}

// Función para inicializar el bus I2C, el control de la tira de LEDs WS2812, y el canal RMT
esp_err_t touch_audio_rmt_init(uint8_t gpio_num, int led_number, uint8_t rmt_channel)
{
    ESP_LOGI(TAG, "Initializing WS2812");  // Log que indica que se está inicializando la tira de LEDs WS2812
    rmt_config_t config = RMT_DEFAULT_CONFIG_TX(gpio_num, rmt_channel);  // Configuración por defecto del canal RMT

    /*!< set counter clock to 40MHz */
    config.clk_div = 2;  // Establece el divisor del reloj a 2

    ESP_ERROR_CHECK(rmt_config(&config));  // Configura el canal RMT
    ESP_ERROR_CHECK(rmt_driver_install(config.channel, 0, 0));  // Instala el driver RMT

    led_strip_config_t strip_config = LED_STRIP_DEFAULT_CONFIG(led_number, (led_strip_dev_t)config.channel);  // Configuración por defecto de la tira de LEDs
    strip = led_strip_new_rmt_ws2812(&strip_config);  // Crea una nueva tira de LEDs WS2812

    if (!strip) {
        ESP_LOGE(TAG, "install WS2812 driver failed");  // Error si la instalación del driver falla
        return ESP_FAIL;
    }

    /*!< Clear LED strip (turn off all LEDs) */
    ESP_ERROR_CHECK(strip->clear(strip, 100));  // Apaga todos los LEDs de la tira
    /*!< Show simple rainbow chasing pattern */
    
    return ESP_OK;
}

// Función para inicializar el sistema SPIFFS (SPI Flash File System)
esp_err_t spiffs_init(void)
{
    esp_err_t ret = ESP_OK;
    ESP_LOGI(TAG, "Initializing SPIFFS");  // Log que indica que se está inicializando SPIFFS

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",  // Ruta base para el sistema de archivos
        .partition_label = NULL,  // Etiqueta de la partición (NULL usa la partición por defecto)
        .max_files = 5,  // Número máximo de archivos
        .format_if_mount_failed = true  // Formatea si el montaje falla
    };

    /*!< Use settings defined above to initialize and mount SPIFFS filesystem. */
    /*!< Note: esp_vfs_spiffs_register is an all-in-one convenience function. */
    ret = esp_vfs_spiffs_register(&conf);  // Registra el sistema de archivos SPIFFS

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");  // Error al montar o formatear SPIFFS
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");  // Error al encontrar la partición SPIFFS
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));  // Otro error al inicializar SPIFFS
        }

        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);  // Obtiene información de la partición SPIFFS

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));  // Error al obtener información de la partición SPIFFS
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);  // Log de la información de la partición SPIFFS
    }

    /*!< Write to file */
    ESP_LOGI(TAG, "Writing to file");  // Log que indica que se está escribiendo en un archivo
    FILE *f = fopen("/spiffs/sta.txt", "w");  // Abre un archivo para escritura

    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing");  // Error al abrir el archivo para escritura
        return ESP_FAIL;
    }

    const char *text = "Estoy escribiendo en el sta.txt!";  // Texto a escribir en el archivo
    size_t written = fprintf(f, "%s\n", text);  // Escribe el texto en el archivo
    ESP_LOGI(TAG, "Written %d characters to file", written);  // Log de los caracteres escritos

    if (ferror(f)) {
        ESP_LOGE(TAG, "Error occurred while writing to file");  // Error al escribir en el archivo
        fclose(f);
        return ESP_FAIL;
    }

    fclose(f);  // Cierra el archivo

    /*!< Open file for reading */
    ESP_LOGI(TAG, "Reading file");  // Log que indica que se está leyendo desde un archivo
    f = fopen("/spiffs/sta.txt", "r");  // Abre un archivo para lectura

    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for reading");  // Error al abrir el archivo para lectura
        return ESP_FAIL;
    }

    char line[64];
    if (fgets(line, sizeof(line), f) == NULL) {
        ESP_LOGE(TAG, "Failed to read from file");  // Error al leer del archivo
        fclose(f);
        return ESP_FAIL;
    }
    
    fclose(f);  // Cierra el archivo

    /*!< strip newline */
    char *pos = strchr(line, '\n');
    if (pos) {
        *pos = '\0';  // Elimina el salto de línea del texto leído
    }

    ESP_LOGI(TAG, "Read from file: '%s'", line);  // Log del texto leído del archivo

    return ESP_OK;
}

// Función principal de aplicación
void app_main()
{
    /*!< Print basic information */
    ESP_LOGI(TAG, "[APP] Startup..");  // Log de inicio de la aplicación
    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());  // Muestra la memoria libre disponible
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());  // Muestra la versión del framework ESP-IDF

    esp_log_level_set("*", ESP_LOG_INFO);  // Configura el nivel de log para todos los componentes
    esp_log_level_set("MQTT_CLIENT", ESP_LOG_VERBOSE);  // Configura el nivel de log para el cliente MQTT
    esp_log_level_set("TRANSPORT_TCP", ESP_LOG_VERBOSE);  // Configura el nivel de log para la capa de transporte TCP
    esp_log_level_set("TRANSPORT_SSL", ESP_LOG_VERBOSE);  // Configura el nivel de log para la capa de transporte SSL
    esp_log_level_set("TRANSPORT", ESP_LOG_VERBOSE);  // Configura el nivel de log para la capa de transporte
    esp_log_level_set("OUTBOX", ESP_LOG_VERBOSE);  // Configura el nivel de log para el buzón de salida de MQTT

    /*!< Initialize NVS */
    esp_err_t ret = nvs_flash_init();  // Inicializa el almacenamiento de datos no volátiles (NVS)

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());  // Borra los datos de NVS si no hay páginas libres o hay una nueva versión
        ret = nvs_flash_init();  // Vuelve a inicializar NVS
    }

    ESP_ERROR_CHECK(esp_netif_init());  // Inicializa la interfaz de red
    ESP_ERROR_CHECK(esp_event_loop_create_default());  // Crea el bucle de eventos por defecto

    ESP_ERROR_CHECK(ret);  // Verifica errores al inicializar NVS

    ESP_ERROR_CHECK(spiffs_init());  // Inicializa el sistema de archivos SPIFFS
    wifi_init_softap();
    mqtt_app_start();  // Inicia el cliente MQTT
    //ESP_ERROR_CHECK(start_webserver());
    start_webserver();
    ESP_ERROR_CHECK(i2c_bus_init());  // Inicializa el bus I2C
    ESP_ERROR_CHECK(touch_audio_rmt_init(CONFIG_EXAMPLE_RMT_TX_GPIO, CONFIG_EXAMPLE_STRIP_LED_NUMBER, RMT_CHANNEL_0));  // Inicializa el control de la tira de LEDs WS2812 y el canal RMT

    /*!< Initialize touch */
    touch_init();  // Inicializa el controlador de pantalla táctil
    /*!< Initialize audio */
    audio_init(strip);  // Inicializa el controlador de audio con la tira de LEDs
}
