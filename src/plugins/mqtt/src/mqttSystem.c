/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#define _DEFAULT_SOURCE
#include "mqttSystem.h"
#include "MQTTAsync.h"
#include "cJson.h"
#include "mqtt.h"
#include "mqttLog.h"
#include "os.h"
#include "string.h"
#include "taos.h"
#include "tglobal.h"
#include "tsclient.h"
#include "tsocket.h"
#include "ttimer.h"

#define CLIENTID "taos"
#define TOPIC "/taos/+/+/+/"  // taos/<token>/<db name>/<table name>/
#define PAYLOAD "Hello World!"
#define QOS 1
#define TIMEOUT 10000L

MQTTAsync                   client;
MQTTAsync_connectOptions    conn_opts = MQTTAsync_connectOptions_initializer;
MQTTAsync_disconnectOptions disc_opts = MQTTAsync_disconnectOptions_initializer;
void*                       mqtt_conn = NULL;
int                         disc_finished = 0;
int                         subscribed = 0;
int                         finished = 0;
int                         can_exit = 0;

void connlost(void* context, char* cause) {
  MQTTAsync                client = (MQTTAsync)context;
  MQTTAsync_connectOptions conn_opts = MQTTAsync_connectOptions_initializer;
  int                      rc;

  mqttError("\nConnection lost");
  if (cause) mqttError("     cause: %s", cause);

  mqttPrint("Reconnecting");
  conn_opts.keepAliveInterval = 20;
  conn_opts.cleansession = 1;
  if ((rc = MQTTAsync_connect(client, &conn_opts)) != MQTTASYNC_SUCCESS) {
    mqttError("Failed to start connect, return code %d", rc);
    finished = 1;
  }
}
char split(char str[], char delims[], char** p_p_cmd_part, int max) {
  char*  token = strtok(str, delims);
  char   part_index = 0;
  char** tmp_part = p_p_cmd_part;
  while (token) {
    *tmp_part++ = token;
    token = strtok(NULL, delims);
    part_index++;
    if (part_index >= max) break;
  }
  return part_index;
}

int msgarrvd(void* context, char* topicName, int topicLen, MQTTAsync_message* message) {
  mqttTrace("Message arrived,topic is %s,message is  %.*s", topicName, message->payloadlen, (char*)message->payload);
  char _token[128] = {0};
  char _dbname[128] = {0};
  char _tablename[128] = {0};
  if (mqtt_conn == NULL) {
    mqttPrint("connect database");
    taos_connect_a(NULL, "monitor", tsInternalPass, "", 0, mqttInitConnCb, &client, &mqtt_conn);
  }
  if (strncmp(topicName, "/taos/", 6) == 0) {
    char* p_p_cmd_part[5] = {0};
    char  copystr[1024] = {0};
    strncpy(copystr, topicName, MIN(1024, strlen(topicName)));
    char part_index = split(copystr, "/", p_p_cmd_part, 10);
    if (part_index < 4) {
      mqttError("The topic %s is't format '%s'.", topicName, TOPIC);
    } else {
      strncpy(_token, p_p_cmd_part[1], 127);
      strncpy(_dbname, p_p_cmd_part[2], 127);
      strncpy(_tablename, p_p_cmd_part[3], 127);
      mqttPrint("part count=%d,access token:%s,database name:%s, table name:%s", part_index, _token, _dbname,
                _tablename);
      cJSON* jPlayload = cJSON_Parse((char*)message->payload);
      char   _names[102400] = {0};
      char   _values[102400] = {0};
      int    i = 0;
      int    count = cJSON_GetArraySize(jPlayload);
      for (; i < count; i++)  //遍历最外层json键值对
      {
        cJSON* item = cJSON_GetArrayItem(jPlayload, i);
        if (cJSON_Object == item->type) {
          mqttPrint("The item '%s' is not supported", item->string);
        } else {
          strcat(_names, item->string);
          if (i < count - 1) {
            strcat(_names, ",");
          }
          char* __value_json = cJSON_Print(item);
          strcat(_values, __value_json);
          free(__value_json);
          if (i < count - 1) {
            strcat(_values, ",");
          }
        }
      }
      cJSON_free(jPlayload);
      char _sql[102400] = {0};
      sprintf(_sql, "INSERT INTO %s.%s (%s) VALUES(%s);", _dbname, _tablename, _names, _values);

      if (mqtt_conn != NULL) {
        mqttPrint("query:%s", _sql);
        taos_query_a(mqtt_conn, _sql, mqtt_query_insert_callback, &client);
      }
    }
  }
  MQTTAsync_freeMessage(&message);
  MQTTAsync_free(topicName);
  return 1;
}
void mqtt_query_insert_callback(void* param, TAOS_RES* result, int32_t code) {
  if (code < 0) {
    mqttError("mqtt:%d, save data failed, code:%s", code, tstrerror(code));
  } else if (code == 0) {
    mqttError("mqtt:%d, save data failed, affect rows:%d", code, code);
  } else {
    mqttPrint("mqtt:%d, save data   success, code:%s", code, tstrerror(code));
  }
}

void onDisconnectFailure(void* context, MQTTAsync_failureData* response) {
  mqttError("Disconnect failed, rc %d\n", response->code);
  disc_finished = 1;
}

void onDisconnect(void* context, MQTTAsync_successData* response) {
  mqttError("Successful disconnection\n");
  if (mqtt_conn != NULL) {
    taos_close(mqtt_conn);
    mqtt_conn = NULL;
  }
  disc_finished = 1;
}

void onSubscribe(void* context, MQTTAsync_successData* response) {
  mqttPrint("Subscribe succeeded\n");
  subscribed = 1;
}

void onSubscribeFailure(void* context, MQTTAsync_failureData* response) {
  mqttError("Subscribe failed, rc %d\n", response->code);
  finished = 1;
}

void onConnectFailure(void* context, MQTTAsync_failureData* response) {
  mqttError("Connect failed, rc %d\n", response->code);
  finished = 1;
}

void onConnect(void* context, MQTTAsync_successData* response) {
  MQTTAsync                 client = (MQTTAsync)context;
  MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
  int                       rc;

  mqttPrint("Successful connection\n");

  mqttPrint("Subscribing to topic %s\nfor client %s using QoS%d", TOPIC, CLIENTID, QOS);
  opts.onSuccess = onSubscribe;
  opts.onFailure = onSubscribeFailure;
  opts.context = client;
  if ((rc = MQTTAsync_subscribe(client, TOPIC, QOS, &opts)) != MQTTASYNC_SUCCESS) {
    mqttError("Failed to start subscribe, return code %d\n", rc);
    finished = 1;
  }
}

int32_t mqttInitSystem() {
  int rc = 0;
  if (strnlen(tsMqttBrokerAddress, 128) == 0) {
    rc = EXIT_FAILURE;
    mqttError("Can't to create client, mqtt broker address is empty %d", rc);
  } else {
    if ((rc = MQTTAsync_create(&client, tsMqttBrokerAddress, CLIENTID, MQTTCLIENT_PERSISTENCE_NONE, NULL)) !=
        MQTTASYNC_SUCCESS) {
      mqttError("Failed to create client, return code %d", rc);
      rc = EXIT_FAILURE;

    } else {
      if ((rc = MQTTAsync_setCallbacks(client, client, connlost, msgarrvd, NULL)) != MQTTASYNC_SUCCESS) {
        mqttError("Failed to set callbacks, return code %d", rc);
        rc = EXIT_FAILURE;
      } else {
        conn_opts.keepAliveInterval = 20;
        conn_opts.cleansession = 1;
        conn_opts.onSuccess = onConnect;
        conn_opts.onFailure = onConnectFailure;
        conn_opts.context = client;
        taos_init();
      }
    }
  }
  return rc;
}

int32_t mqttStartSystem() {
  int rc = 0;
  mqttPrint("mqttStartSystem");
  if ((rc = MQTTAsync_connect(client, &conn_opts)) != MQTTASYNC_SUCCESS) {
    mqttError("Failed to start connect, return code %d", rc);
    rc = EXIT_FAILURE;
  } else {
    while (!subscribed && !finished) usleep(10000L);
    disc_opts.onSuccess = onDisconnect;
    disc_opts.onFailure = onDisconnectFailure;
    mqttPrint("Successful started\n");
  }
  return rc;
}

void mqttInitConnCb(void* param, TAOS_RES* result, int32_t code) {
  if (code < 0) {
    mqttError("mqtt:%d, connect to database failed, reason:%s", code, tstrerror(code));
    taos_close(mqtt_conn);
    mqtt_conn = NULL;
    return;
  }
  mqttTrace("mqtt:%d, connect to database success, reason:%s", code, tstrerror(code));
}

void mqttStopSystem() {
  int rc = 0;
  if ((rc = MQTTAsync_disconnect(client, &disc_opts)) != MQTTASYNC_SUCCESS) {
    mqttError("Failed to start disconnect, return code %d", rc);
    rc = EXIT_FAILURE;
  } else {
    while (!disc_finished) {
      usleep(10000L);
    }
  }
  taos_close(mqtt_conn);
}

void mqttCleanUpSystem() {
  mqttPrint("mqttCleanUpSystem");
  MQTTAsync_destroy(&client);
  taos_cleanup(mqtt_conn);
}
