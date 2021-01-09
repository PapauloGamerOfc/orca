#include <time.h> //for clock_gettime()
#include <math.h> //for lround()
#include <stdio.h>
#include <stdlib.h>

#include <libdiscord.h>
#include "discord-common.h"
#include "curl-websocket.h"

#define BASE_WEBSOCKETS_URL "wss://gateway.discord.gg/?v=6&encoding=json"

static char*
_payload_strevent(enum ws_opcode opcode)
{

//if case matches return token as string
#define CASE_RETURN_STR(opcode) case opcode: return #opcode

  switch(opcode) {
    CASE_RETURN_STR(GATEWAY_DISPATCH);
    CASE_RETURN_STR(GATEWAY_HEARTBEAT);
    CASE_RETURN_STR(GATEWAY_IDENTIFY);
    CASE_RETURN_STR(GATEWAY_PRESENCE_UPDATE);
    CASE_RETURN_STR(GATEWAY_VOICE_STATE_UPDATE);
    CASE_RETURN_STR(GATEWAY_RESUME);
    CASE_RETURN_STR(GATEWAY_RECONNECT);
    CASE_RETURN_STR(GATEWAY_REQUEST_GUILD_MEMBERS);
    CASE_RETURN_STR(GATEWAY_INVALID_SESSION);
    CASE_RETURN_STR(GATEWAY_HELLO);
    CASE_RETURN_STR(GATEWAY_HEARTBEAT_ACK);

  default:
    ERROR("Invalid WebSockets opcode (code: %d)", opcode);
  }
}

static long
_timestamp_ms()
{
  struct timespec t;
  clock_gettime(CLOCK_REALTIME, &t);

  return t.tv_sec*1000 + lround(t.tv_nsec/1.0e6);
}

static void
_ws_send_identify(struct discord_ws_s *ws)
{
  D_PRINT("IDENTIFY PAYLOAD:\n\t%s", ws->identify);

  bool ret = cws_send_text(ws->ehandle, ws->identify);
  ASSERT_S(true == ret, "Couldn't send identify payload");
}

static void
_discord_on_hello(struct discord_ws_s *ws)
{
  ws->status = WS_CONNECTED;

  ws->hbeat.interval_ms = 0;
  ws->hbeat.start_ms = _timestamp_ms();

  jscon_scanf(ws->payload.event_data, "%ld[heartbeat_interval]", &ws->hbeat.interval_ms);
  ASSERT_S(ws->hbeat.interval_ms > 0, "Invalid heartbeat_ms");

  _ws_send_identify(ws);
}

static void
_discord_on_dispatch(struct discord_ws_s *ws)
{
  if (ws->cbs.on_ready
      && !strcmp("READY", ws->payload.event_name))
  {
    (*ws->cbs.on_ready)((discord_t*)ws);
  }/*
  else if (ws->cbs.on_message
           && !strcmp("MESSAGE", ws->payload.event_name))
  {
    (*ws->cbs.on_message)((discord_t*)ws,);
  }*/
  else {
    ERROR("Unknown GATEWAY_DISPATCH event: %s", ws->payload.event_name);
  }
}

static void
_ws_on_connect_cb(void *data, CURL *ehandle, const char *ws_protocols)
{
  D_PRINT("Connected, WS-Protocols: '%s'", ws_protocols);

  (void)data;
  (void)ehandle;
  (void)ws_protocols;
}

static void
_ws_on_close_cb(void *data, CURL *ehandle, enum cws_close_reason cwscode, const char *reason, size_t len)
{
    struct discord_ws_s *ws = data;
    ws->status = WS_DISCONNECTED;

    D_PRINT("CLOSE=%4d %zd bytes '%s'", cwscode, len, reason);

    (void)ehandle;
    (void)cwscode;
    (void)len;
    (void)reason;
}

static void
_ws_on_text_cb(void *data, CURL *ehandle, const char *text, size_t len)
{
  struct discord_ws_s *ws = data;

  D_PRINT("ON_TEXT:\n\t\t%s", text);

  jscon_scanf((char*)text, 
              "%s[t]" \
              "%d[s]" \
              "%d[op]" \
              "%S[d]",
               ws->payload.event_name,
               &ws->payload.seq_number,
               &ws->payload.opcode,
               ws->payload.event_data);

  D_NOTOP_PRINT("OP:\t\t%s\n\t" \
                "EVENT_NAME:\t%s\n\t" \
                "SEQ_NUMBER:\t%d\n\t" \
                "EVENT_DATA:\t%s", 
                _payload_strevent(ws->payload.opcode), 
                *ws->payload.event_name //if event name exists
                   ? ws->payload.event_name //prints event name
                   : "UNDEFINED_EVENT", //otherwise, print this
                ws->payload.seq_number,
                ws->payload.event_data);

  switch (ws->payload.opcode){
  case GATEWAY_HELLO:
      _discord_on_hello(ws);
      break;
  case GATEWAY_DISPATCH:
      _discord_on_dispatch(ws);
      break;
  case GATEWAY_RECONNECT:
      break;
  case GATEWAY_HEARTBEAT_ACK:
      break; 
  default:
      ERROR("Invalid Discord Gateway opcode (code: %d)", ws->payload.opcode);
  }

  (void)len;
  (void)ehandle;
}

/* init easy handle with some default opt */
static CURL*
_discord_easy_init(struct discord_ws_s *ws)
{
  //missing on_binary, on_ping, on_pong
  struct cws_callbacks cws_cbs = {
    .on_connect = &_ws_on_connect_cb,
    .on_text = &_ws_on_text_cb,
    .on_close = &_ws_on_close_cb,
    .data = ws,
  };

  CURL *new_ehandle = cws_new(BASE_WEBSOCKETS_URL, NULL, &cws_cbs);
  ASSERT_S(NULL != new_ehandle, "Out of memory");

  CURLcode ecode;
  D_ONLY(ecode = curl_easy_setopt(new_ehandle, CURLOPT_VERBOSE, 2L));
  D_ASSERT_S(CURLE_OK == ecode, curl_easy_strerror(ecode));

  ecode = curl_easy_setopt(new_ehandle, CURLOPT_FOLLOWLOCATION, 2L);
  ASSERT_S(CURLE_OK == ecode, curl_easy_strerror(ecode));

  return new_ehandle;
}

static CURLM*
_discord_multi_init()
{
  CURLM *new_mhandle = curl_multi_init();
  ASSERT_S(NULL != new_mhandle, "Out of memory");

  return new_mhandle;
}

//@todo allow for user input
static char*
_discord_identify_init(char token[])
{
  const char fmt_properties[] = \
    "{\"$os\":\"%s\",\"$browser\":\"libdiscord\",\"$device\":\"libdiscord\"}";
  const char fmt_presence[] = \
    "{\"since\":%s,\"activities\":%s,\"status\":\"%s\",\"afk\":%s}";
  const char fmt_event_data[] = \
    "{\"token\":\"%s\",\"intents\":%d,\"properties\":%s,\"presence\":%s}";
  const char fmt_identify[] = \
    "{\"op\":2,\"d\":%s}"; //op:2 means GATEWAY_IDENTIFY

  //https://discord.com/developers/docs/topics/gateway#identify-identify-connection-properties
  /* @todo $os detection */
  char properties[512];
  snprintf(properties, sizeof(properties)-1, fmt_properties, "Linux");

  //https://discord.com/developers/docs/topics/gateway#sharding
  /* @todo */

  //https://discord.com/developers/docs/topics/gateway#update-status-gateway-status-update-structure
  char presence[512];
  snprintf(presence, sizeof(presence)-1, fmt_presence, 
           "null", "null", "online", "false");

  //https://discord.com/developers/docs/topics/gateway#identify-identify-structure
  char event_data[512];
  int len = sizeof(fmt_identify);
  len += snprintf(event_data, sizeof(event_data)-1, fmt_event_data,
                  token, GUILD_MESSAGES, properties, presence);

  char *identify = malloc(len);
  ASSERT_S(NULL != identify, "Out of memory");

  snprintf(identify, len-1, fmt_identify, event_data);

  return identify;
}

void
Discord_ws_init(struct discord_ws_s *ws, char token[])
{
  ws->status = WS_DISCONNECTED;

  ws->identify = _discord_identify_init(token);
  ws->ehandle = _discord_easy_init(ws);
  ws->mhandle = _discord_multi_init();

  ws->cbs.on_ready = NULL;
  ws->cbs.on_message = NULL;
}

void
Discord_ws_cleanup(struct discord_ws_s *ws)
{
  free(ws->identify);
  curl_multi_cleanup(ws->mhandle);
  cws_free(ws->ehandle);
}

/* send heartbeat pulse to websockets server in order
 *  to maintain connection alive */
static void
_ws_send_heartbeat(struct discord_ws_s *ws)
{
  char str[64];

  if (!ws->payload.seq_number)
    snprintf(str, sizeof(str)-1, "{\"op\":1,\"d\":null}");
  else
    snprintf(str, sizeof(str)-1, "{\"op\": 1,\"d\":%d}", ws->payload.seq_number);

  D_PRINT("HEARTBEAT_PAYLOAD:\n\t\t%s", str);
  bool ret = cws_send_text(ws->ehandle, str);
  ASSERT_S(true == ret, "Couldn't send heartbeat payload");

  ws->hbeat.start_ms = _timestamp_ms();
}

/* main websockets event loop */
static void
_ws_main_loop(struct discord_ws_s *ws)
{
  int is_running = 0;

  curl_multi_perform(ws->mhandle, &is_running);

  CURLMcode mcode;
  do {
    int numfds;

    mcode = curl_multi_perform(ws->mhandle, &is_running);
    ASSERT_S(CURLM_OK == mcode, curl_multi_strerror(mcode));
    
    //wait for activity or timeout
    mcode = curl_multi_poll(ws->mhandle, NULL, 0, 1000, &numfds);
    ASSERT_S(CURLM_OK == mcode, curl_multi_strerror(mcode));

    /*check if timespan since first pulse is greater than
     * minimum heartbeat interval required*/
    if ((WS_CONNECTED == ws->status)
        && 
        (ws->hbeat.interval_ms < (_timestamp_ms() - ws->hbeat.start_ms))) 
    {
      _ws_send_heartbeat(ws);
    }
  } while(is_running);
}

/* connects to the discord websockets server */
void
Discord_ws_run(struct discord_ws_s *ws)
{
  curl_multi_add_handle(ws->mhandle, ws->ehandle);
  _ws_main_loop(ws);
  curl_multi_remove_handle(ws->mhandle, ws->ehandle);
}

void
Discord_ws_set_on_ready(struct discord_ws_s *ws, discord_onrdy_cb *user_cb){
  ws->cbs.on_ready = user_cb;
}

void
Discord_ws_set_on_message(struct discord_ws_s *ws, discord_onmsg_cb *user_cb){
  ws->cbs.on_message = user_cb;
}