#include <esp_wifi.h>               //Conexão Wireless
#include "esp_log.h"                //log de eventos no monitor serial
#include <esp_http_server.h>        //biblioteca para poder usar o server http
#include "freertos/event_groups.h"  //grupo de eventos
#include "nvs_flash.h"              //memória nvs
#include "driver/ledc.h"            //PWM
#include <math.h>                   //Função log
#include <sys/param.h>              //Função MIN
#include <string.h>





/*------------------------Mapeamento de Hardware----------------------*/
//Vamos definir quais pinos do ESP serão usados para ler o ADC e acionar o rele
#define pino_ADC    12
#define pino_rele   15



/*------------------------Definições de Projeto-----------------------*/
/* O event Group do wireless permite muitos eventos por grupo mas nos só nos importamos com 2 eventos:
 * - Conseguimos conexão com o AP com um IP
 * - Falhamos na conexão apos o número máximo de tentativas*/
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1


/*Aqui definimos o SSID, a Senha e o número máximo de tentativas que vamos 
tentar ao se conectar ao access point da rede wireless*/
#define ESP_WIFI_SSID      "tira_o_zoio"
#define ESP_WIFI_PASS      "jabuticaba"
#define ESP_MAXIMUM_RETRY  10



/*-----------------------------------------------Constantes de Projeto --------------------------------------*/

static const char *TAG = "LOG";             //A tag que será impressa no log do sistema 



/*----------------------------------------------------Objetos------------------------------------------------*/

//Handle dos Eventos Wireless
static EventGroupHandle_t s_wifi_event_group;

//Handle do server http
static httpd_handle_t server =NULL;


struct parametros_irrigacao{
    bool estado;
    int horario;
    int umidade_minima;
    int umidade_maxima;

}parametros_irrigacao;

/*-------------------------------------------------Variáveis Globais-----------------------------------------*/
static struct parametros_irrigacao manha={1,0600,40,70};
static struct parametros_irrigacao tarde={0,1800,30,70};

int umidade_atual = 57;

/*-----------------------------------------------Declaração das Funções--------------------------------------*/
static void setup_nvs();     //Inicia a memória nvs. Ela é necessária para se conectar à rede Wireless

void wifi_init_sta(); //Configura a rede wireless

/* Lida com os Eventos da rede Wireless(reconexão, IPs, etc), essa função é ativada durante a chamada de 
 * void wifi_init_sta() e permanece monitorando os eventos de rede e imprimindo no terminal
 * as tentativas de conexão e o endereço IP recebido que o ESP ganhou*/
static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data);

//Cria o Server, Faz as configurações Padrão e Inicia os URI Handlers para os GETs
static httpd_handle_t start_webserver(void);


//Imprimimos a Webpage
static void print_webpage(httpd_req_t *req);

//handler do Get da Página Principal
static esp_err_t main_page_get_handler(httpd_req_t *req);

//handler do formulário html do pwm0
static esp_err_t pwm_post_handler(httpd_req_t *req);

static void processa_post_request(char *content);

static void cria_delay(void *pvParameter);


/*--------------------------------------Declaração dos GETs do http------------------------------------------*/
//a declaração do GET da página Principal
static const httpd_uri_t main_page = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = main_page_get_handler,
    .user_ctx  = NULL
};


// URI handler do formulário
static const httpd_uri_t post_pwm = {
    .uri      = "/",
    .method   = HTTP_POST,
    .handler  = pwm_post_handler,
    .user_ctx = NULL
};


/*-----------------------------------------Função Main--------------------------------------------------------*/

void app_main() {
    setup_nvs();                    //inicia a memória nvs necessária para uso do wireless
    wifi_init_sta();                //inicia o wireless e se conecta à rede
    server = start_webserver();     //configura e inicia o server
   // atualiza_PWM(0,5, 30);
   // atualiza_PWM(1,10, 70);
    xTaskCreate(&cria_delay, "cria_delay", 512,NULL,5,NULL );

}


/*-------------------------------Implementação das Funções Auxiliares-----------------------------------------*/

/*----Inicializa a memória nvs pois é necessária para o funcionamento do Wireless---------*/
static void setup_nvs()
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

/*----------------Lida com os Eventos da rede Wireless, conexão à rede e endereço IP-----------------------*/
//essa função é executada em segundo plano
static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    static uint32_t numero_tentativa_de_conexao_wifi = 0;//numero atual da tentativa de se conectar a rede,
                                                         //tentativas máximas= EXAMPLE_ESP_MAXIMUM_RETRY
    
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect(); //se o Wifi ja foi iniciado tenta se conectar
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (numero_tentativa_de_conexao_wifi < ESP_MAXIMUM_RETRY) { //se o numero atual de tentativas
            esp_wifi_connect();                                             //de conexão não atingiu o máximo
            numero_tentativa_de_conexao_wifi++;                             //tenta denovo            
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);          //se o numero atingiu o máximo ativa
        }                                                                   //o envento de falha no wifi            
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) { //se estamos conectados a rede vamos
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;         //imprimir o IP no terminal via ESP_LOGI()
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        numero_tentativa_de_conexao_wifi = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}


/*---------------------------Inicializa a Conexão Wireless-------------------------*/
void wifi_init_sta()
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = ESP_WIFI_SSID,
            .password = ESP_WIFI_PASS,
            /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by commenting below line */
	     .threshold.authmode = WIFI_AUTH_WPA2_PSK,

            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 ESP_WIFI_SSID, ESP_WIFI_PASS);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 ESP_WIFI_SSID, ESP_WIFI_PASS);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }

    /* The event will not be processed after unregister */
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
    vEventGroupDelete(s_wifi_event_group);
}

/*--------------Cria o Server, Faz as configurações Padrão e Inicia os URI Handlers--------------*/
static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server   = NULL;
    httpd_config_t config   = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    // Inicia o server http
    printf("Iniciando o Server na Porta: '%d'\n", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        // Set URI handlers
        printf("Registrando URI handlers\n");
        httpd_register_uri_handler(server, &main_page);
        httpd_register_uri_handler(server, &post_pwm);
        return server;
    }

    printf("Erro ao iniciar Server\n");
    return NULL;
}

/*---Essa função concatena a página web como um vetor char e a envia como resposta da requisição req---*/
static void print_webpage(httpd_req_t *req)
{

    char *buffer;
    buffer = (char *) malloc(4000);
    
    const char *index_html_part1= "<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><link rel=\"icon\" href=\"data:,\"></head><title>Projeto 10 - Gerador PWM controlado via Wireless</title><link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\" /><body><h2>Projeto 13 - Horta</h2><h3>Umidade Atual: ";
    char c_umidade_atual[6];
    sprintf(c_umidade_atual, "\"%d\"", umidade_atual);

    const char *index_html_part2= "</h3><div class=\"wrap\"><div class=\"fleft\"><h3>Rodador de manhã</h3><form class=\"formulario\" method=\"post\"><label>Horário de ligar </label><input class=\"campo_hora\" type=\"time\" id=\"hora\" name=\"freq\" value=";
    char c_horario_manha[6];
    sprintf(c_horario_manha, "\"%d\"", manha.horario);
    
    const char *index_html_part3="autocomplete=\"off\"><label><br><br></label><label>Umidade mínima:&#160; </label><input class=\"campo_duty\" type=\"range\" min=\"0\" max=\"100\" value=";
    char c_umidade_min_manha[6];
    sprintf(c_umidade_min_manha, "\"%d\"", manha.umidade_minima);

    const char *index_html_part4="id=\"myRange\"><label> <span id=\"demo\"></span>%</label><br><br><label>Umidade máxima:&#160; </label><input class=\"campo_duty\" type=\"range\" min=\"0\" max=\"100\" value=";
    char c_umidade_max_manha[6];
    sprintf(c_umidade_max_manha, "\"%d\"", manha.umidade_maxima);

    const char *index_html_part5="id=\"myRange2\"><label> <span id=\"demo2\"></span>%</label><br><br><input name=\"estado_ligado\" class= \"campo_saida\" type=\"radio\" ";

    const char *pwmchecked="checked=\"checked\""; //<!--if(estado_ligado_manha)--
    
    const char *index_html_part6="><label>Ligado </label><input name=\"estado_desligado\" class= \"campo_saida\" type=\"radio\"";

    const char *index_html_part7="><label>Desligado </label><br><br><input class=\"btn_submit\" type=\"submit\" value=\"OK\" name=\"pwm0\"></form></div><div class=\"fright\"><h3>Rodador de tarde</h3><form class=\"formulario\" method=\"post\"><label>Horário de ligar </label><input class=\"campo_hora\" type=\"time\" id=\"hora\" name=\"freq\" value=";

    char c_horario_tarde[8];
    sprintf(c_horario_tarde,    "\"%d\"", tarde.horario);

    const char *index_html_part8="autocomplete=\"off\"><label><br><br></label><label>Umidade mínima:&#160; </label><input class=\"campo_duty\" type=\"range\" min=\"0\" max=\"100\" value=";
    char c_umidade_min_tarde[6];
    sprintf(c_umidade_min_tarde, "\"%d\"", tarde.umidade_minima);
    
    const char *index_html_part9="id=\"myRange3\"><label> <span id=\"demo3\"></span>%</label><br><br><label>Umidade máxima:&#160; </label><input class=\"campo_duty\" type=\"range\" min=\"0\" max=\"100\" value=";
    char c_umidade_max_tarde[6];
    sprintf(c_umidade_max_tarde, "\"%d\"", tarde.umidade_maxima);


    const char *index_html_part10="id=\"myRange4\"><label> <span id=\"demo4\"></span>%</label><br><br><input name=\"estado_ligado\" class= \"campo_saida\" type=\"radio\"";

    const char *index_html_part11="><label>Ligado </label><input name=\"estado_desligado\" class= \"campo_saida\" type=\"radio\" ";

    const char *index_html_part12="><label>Desligado </label><br><br><input class=\"btn_submit\" type=\"submit\" value=\"OK\" name=\"pwm0\"></form></div></div><script>var slider = document.getElementById(\"myRange\");var output = document.getElementById(\"demo\");output.innerHTML = slider.value;slider.oninput = function() {output.innerHTML = this.value;};var slider2 = document.getElementById(\"myRange2\");var output2 = document.getElementById(\"demo2\");output2.innerHTML = slider2.value;slider2.oninput = function() {output2.innerHTML = this.value;};var slider3 = document.getElementById(\"myRange3\");var output3 = document.getElementById(\"demo3\");output3.innerHTML = slider3.value;slider3.oninput = function() ";
    const char *index_html_part13="{output3.innerHTML = this.value;};var slider4 = document.getElementById(\"myRange4\");var output4 = document.getElementById(\"demo4\");output4.innerHTML = slider4.value;slider4.oninput = function() {output4.innerHTML = this.value;}</script></body></html>";



    strcpy(buffer, index_html_part1);
    strcat(buffer, c_umidade_atual);
    strcat(buffer, index_html_part2);
    strcat(buffer, c_horario_manha);
    strcat(buffer, index_html_part3);
    strcat(buffer, c_umidade_min_manha);
    strcat(buffer, index_html_part4);
    strcat(buffer, c_umidade_max_manha);
    strcat(buffer, index_html_part5);
    if(manha.estado)
        strcat(buffer, pwmchecked);
    
    strcat(buffer, index_html_part6);
    if(!manha.estado)
        strcat(buffer, pwmchecked);
    
    strcat(buffer, index_html_part7);
    strcat(buffer, c_horario_tarde);
    strcat(buffer, index_html_part8);
    strcat(buffer, c_umidade_min_tarde);
    strcat(buffer, index_html_part9);
    strcat(buffer, c_umidade_max_tarde);
    strcat(buffer, index_html_part10);
    if(tarde.estado)
        strcat(buffer, pwmchecked);
    
    strcat(buffer, index_html_part11);
    if(!tarde.estado)
        strcat(buffer, pwmchecked);
    
    strcat(buffer, index_html_part12);
    strcat(buffer, index_html_part13);


    httpd_resp_send(req, buffer, strlen(buffer)); //envia via http o buffer que contém a página html completa

    vTaskDelay(3000 / portTICK_RATE_MS);
    free(buffer);

}

/*--------------handler do Get da Página Principal html-------------------*/
static esp_err_t main_page_get_handler(httpd_req_t *req)
{
    //imprime a página
    print_webpage(req);
    //retorna OK
    return ESP_OK;
}

static esp_err_t pwm_post_handler(httpd_req_t *req)
{

     /* Destination buffer for content of HTTP POST request.
     * httpd_req_recv() accepts char* only, but content could
     * as well be any binary data (needs type casting).
     * In case of string data, null termination will be absent, and
     * content length would give length of string */
    char content[100];
    ESP_LOGI(TAG,"pre recv");
    ESP_LOGI(TAG,"manha parametros %d %d %d %d",manha.estado,manha.horario,manha.umidade_minima,manha.umidade_maxima);
    ESP_LOGI(TAG,"tarde parametros %d %d %d %d",tarde.estado,tarde.horario,tarde.umidade_minima,tarde.umidade_maxima);
    
    /* Truncate if content length larger than the buffer */
    size_t recv_size = MIN(req->content_len, sizeof(content));

    int ret = httpd_req_recv(req, content, recv_size);
    if (ret <= 0) {  /* 0 return value indicates connection closed */
        /* Check if timeout occurred */
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            /* In case of timeout one can choose to retry calling
             * httpd_req_recv(), but to keep it simple, here we
             * respond with an HTTP 408 (Request Timeout) error */
            httpd_resp_send_408(req);
        }
        /* In case of error, returning ESP_FAIL will
         * ensure that the underlying socket is closed */
        return ESP_FAIL;
    }
    ESP_LOGI(TAG,"Vetor resposta: %s",content);
    processa_post_request(content);
    ESP_LOGI(TAG,"manha parametros %d %d %d %d",manha.estado,manha.horario,manha.umidade_minima,manha.umidade_maxima);
    ESP_LOGI(TAG,"tarde parametros %d %d %d %d",tarde.estado,tarde.horario,tarde.umidade_minima,tarde.umidade_maxima);
   // atualiza_PWM(pwm[0].estado,pwm[0].frequencia, pwm[0].percentual_duty);
   // atualiza_PWM(pwm[1].estado,pwm[1].frequencia, pwm[1].percentual_duty);
    /* Send a simple response */
    ESP_LOGI(TAG,"post pwm");
    print_webpage(req);
    return ESP_OK;
}

static void processa_post_request(char *content)
{
   /* int index;
    if (strstr(content, "pwm0"))
         index=0;
    else 
         index=1;

    if (strstr(content, "desligado"))
        pwm[index].estado=0;
    else
        pwm[index].estado=1;
     
    //O numero de dígitos que a variável frequência possui
    int tamanho_freq=strchr(strstr(content, "freq"), '&')- strchr(strstr(content, "freq"), '=')-1;
    //variavel para armazenar a string da frequencia
    char frequencia[10];
    //copia a frequencia que chegou no vetor 'content' para a string
    strncpy(frequencia, (strchr(strstr(content, "freq"), '=')+1), tamanho_freq);
    //converte a string frequencia em int e salva
    pwm[index].frequencia=atoi(frequencia);

    //O numero de dígitos que a variável percentual_duty possui
    int tamanho_duty=strchr(strstr(content, "duty"), '&')- strchr(strstr(content, "duty"), '=')-1;
    //variavel para armazenar a string do duty
    char duty[10];
    //copia o duty que chegou no vetor 'content' para a string
    strncpy(duty, (strchr(strstr(content, "duty"), '=')+1), tamanho_duty);
    //converte a string duty em int e salva
    pwm[index].percentual_duty=atoi(duty);
    */
}


static void cria_delay(void *pvParameter)
{
    while(1)
    {
        vTaskDelay(100 / portTICK_RATE_MS);
    }
    
}