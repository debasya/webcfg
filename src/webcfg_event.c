/*
 * Copyright 2020 Comcast Cable Communications Management, LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "webcfg_multipart.h"
#include "webcfg_notify.h"
#include "webcfg_generic.h"
#include "webcfg.h"
#include <pthread.h>
#include <cJSON.h>
#include <unistd.h>
#include "webcfg_event.h"
#include "webcfg_db.h"
#include "webcfg_param.h"
#include "webcfg_blob.h"
/*----------------------------------------------------------------------------*/
/*                                   Macros                                   */
/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/
/*                               Data Structures                              */
/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
/*                            File Scoped Variables                           */
/*----------------------------------------------------------------------------*/
static pthread_t EventThreadId=0;
static pthread_t processThreadId = 0;
pthread_mutex_t event_mut=PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t event_con=PTHREAD_COND_INITIALIZER;
event_data_t *eventDataQ = NULL;
static expire_timer_t * g_timer_head = NULL;
static int numOfEvents = 0;
/*----------------------------------------------------------------------------*/
/*                             Function Prototypes                            */
/*----------------------------------------------------------------------------*/
void* blobEventHandler();
int parseEventData();
void* processSubdocEvents();

int checkWebcfgTimer();
int addToEventQueue(char *buf);
void sendSuccessNotification(char *name, uint32_t version, uint16_t txid);
WEBCFG_STATUS startWebcfgTimer(char *name, uint16_t transID, uint32_t timeout);
WEBCFG_STATUS stopWebcfgTimer(char *name, uint16_t trans_id);
int checkTimerExpired (char **exp_doc);
void createTimerExpiryEvent(char *docName, uint16_t transid);
WEBCFG_STATUS updateTimerList(int status, char *docname, uint16_t transid, uint32_t timeout);
WEBCFG_STATUS deleteFromTimerList(char* doc_name);
WEBCFG_STATUS checkDBVersion(char *docname, uint32_t version);
WEBCFG_STATUS validateEvent(char *docname, uint16_t txid);
/*----------------------------------------------------------------------------*/
/*                             External Functions                             */
/*----------------------------------------------------------------------------*/

expire_timer_t * get_global_timer_node(void)	
{	
    return g_timer_head;	
}	

//Webcfg thread listens for blob events from respective components.
void initEventHandlingTask()
{
	int err = 0;
	err = pthread_create(&EventThreadId, NULL, blobEventHandler, NULL);
	if (err != 0)
	{
		WebcfgError("Error creating Webconfig event handling thread :[%s]\n", strerror(err));
	}
	else
	{
		WebcfgInfo("Webconfig event handling thread created Successfully\n");
	}

}

void* blobEventHandler()
{
	pthread_detach(pthread_self());

	int ret = 0;
	char *expired_doc= NULL;
	uint16_t tx_id = 0;

	ret = registerWebcfgEvent(webcfgCallback);
	if(ret)
	{
		WebcfgInfo("registerWebcfgEvent success\n");
	}
	else
	{
		WebcfgError("registerWebcfgEvent failed\n");
	}

	/* Loop to check timer expiry. When timer is not running, loop is active but wont check expiry until next timer starts. */
	while(1)
	{
		if (checkTimerExpired (&expired_doc))
		{
			if(expired_doc !=NULL)
			{
				WebcfgError("Timer expired_doc %s. No event received within timeout period\n", expired_doc);
				//reset timer. Generate internal EXPIRE event with new trans_id and retry.
				tx_id = generateTransactionId(1001,3000);
				WebcfgDebug("EXPIRE event tx_id generated is %lu\n", (long)tx_id);
				updateTimerList(false, expired_doc, tx_id, 0);
				createTimerExpiryEvent(expired_doc, tx_id);
				WebcfgDebug("After createTimerExpiryEvent\n");
			}
			else
			{
				WebcfgError("Failed to get expired_doc\n");
			}
		}
		else
		{
			WebcfgDebug("Waiting at timer loop of 5s\n");
			sleep(5);
		}
	}
	return NULL;
}

//Call back function to be executed when webconfigSignal signal is received from component.
void webcfgCallback(char *Info, void* user_data)
{
	char *buff = NULL;
	WebcfgInfo("Received webconfig event signal Info %s\n", Info);
	WebcfgDebug("user_data %s\n", (char*) user_data);

	buff = strdup(Info);
	addToEventQueue(buff);

	WebcfgDebug("After addToEventQueue\n");
}

//Producer adds event data to queue
int addToEventQueue(char *buf)
{
	event_data_t *Data;

	WebcfgDebug ("Add data to event queue\n");
        Data = (event_data_t *)malloc(sizeof(event_data_t));

	if(Data)
        {
            Data->data =buf;
            Data->next=NULL;
            pthread_mutex_lock (&event_mut);
            if(eventDataQ == NULL)
            {
                eventDataQ = Data;

                WebcfgInfo("Producer added Data\n");
                pthread_cond_signal(&event_con);
                pthread_mutex_unlock (&event_mut);
                WebcfgDebug("mutex unlock in producer event thread\n");
            }
            else
            {
                event_data_t *temp = eventDataQ;
                while(temp->next)
                {
                    temp = temp->next;
                }
                temp->next = Data;
                pthread_mutex_unlock (&event_mut);
            }
        }
        else
        {
            WebcfgError("failure in allocation for event data\n");
        }

	return 0;
}


//Webcfg consumer thread to process the events.
void processWebcfgEvents()
{
	int err = 0;
	err = pthread_create(&processThreadId, NULL, processSubdocEvents, NULL);
	if (err != 0)
	{
		WebcfgError("Error creating processWebcfgEvents thread :[%s]\n", strerror(err));
	}
	else
	{
		WebcfgInfo("processWebcfgEvents thread created Successfully\n");
	}

}

//Parse and process sub doc event data.
void* processSubdocEvents()
{
	event_params_t *eventParam = NULL;
	int rv = WEBCFG_FAILURE;
	WEBCFG_STATUS rs = WEBCFG_FAILURE;
	uint32_t docVersion = 0;
	char err_details[512] = {0};

	while(1)
	{
		pthread_mutex_lock (&event_mut);
		WebcfgDebug("mutex lock in event consumer thread\n");
		if(eventDataQ != NULL)
		{
			event_data_t *Data = eventDataQ;
			eventDataQ = eventDataQ->next;
			pthread_mutex_unlock (&event_mut);
			WebcfgDebug("mutex unlock in event consumer thread\n");

			WebcfgInfo("Data->data is %s\n", Data->data);
			rv = parseEventData(Data->data, &eventParam);
			if(rv == WEBCFG_SUCCESS)
			{
				WebcfgInfo("Event detection\n");
				if (((eventParam->status !=NULL)&&(strcmp(eventParam->status, "ACK")==0)) && (eventParam->timeout == 0))
				{
					WebcfgInfo("ACK event. doc apply success, proceed to add to DB\n");
					if( validateEvent(eventParam->subdoc_name, eventParam->trans_id) == WEBCFG_SUCCESS)
					{
						//version in event &tmp are not same indicates latest doc is not yet applied
						if((getDocVersionFromTmpList(eventParam->subdoc_name))== eventParam->version)
						{
							stopWebcfgTimer(eventParam->subdoc_name, eventParam->trans_id);

							//add to DB, update tmp list and notification based on success ack.
							sendSuccessNotification(eventParam->subdoc_name, eventParam->version, eventParam->trans_id);
							WebcfgInfo("AddToDB subdoc_name %s version %lu\n", eventParam->subdoc_name, (long)eventParam->version);
							checkDBList(eventParam->subdoc_name,eventParam->version);
							WebcfgDebug("checkRootUpdate\n");
							if(checkRootUpdate() == WEBCFG_SUCCESS)
							{
								WebcfgDebug("updateRootVersionToDB\n");
								updateRootVersionToDB();
							}
							addNewDocEntry(get_successDocCount());
						}
						else
						{
							WebcfgError("ACK event version and tmp cache version are not same\n");
						}
					}
				}
				else if (((eventParam->status !=NULL)&&(strcmp(eventParam->status, "NACK")==0)) && (eventParam->timeout == 0))
				{
					WebcfgError("NACK event. doc apply failed for %s\n", eventParam->subdoc_name);
					if( validateEvent(eventParam->subdoc_name, eventParam->trans_id) == WEBCFG_SUCCESS)
					{
						if((getDocVersionFromTmpList(eventParam->subdoc_name))== eventParam->version)
						{
							stopWebcfgTimer(eventParam->subdoc_name, eventParam->trans_id);
							snprintf(err_details, sizeof(err_details),"NACK:%s,%s",((NULL != eventParam->process_name) ? eventParam->process_name : "unknown"), ((NULL != eventParam->failure_reason) ? eventParam->failure_reason : "unknown"));
							WebcfgError("err_details : %s, err_code : %lu\n", err_details, (long) eventParam->err_code);
							updateTmpList(eventParam->subdoc_name, eventParam->version, "failed", err_details, eventParam->err_code, eventParam->trans_id, 0);
							WebcfgInfo("get_global_transID is %s\n", get_global_transID());
							addWebConfgNotifyMsg(eventParam->subdoc_name, eventParam->version, "failed", err_details, get_global_transID(),eventParam->timeout, "status", eventParam->err_code);
						}
						else
						{
							WebcfgError("NACK event version and tmp cache version are not same\n");
						}
					}
				}
				else if ((eventParam->status !=NULL)&&(strcmp(eventParam->status, "EXPIRE")==0))
				{
					WebcfgInfo("EXPIRE event. doc apply timeout expired, need to retry\n");
					WebcfgDebug("get_global_transID is %s\n", get_global_transID());
					if(eventParam->version !=0)
					{
						docVersion = eventParam->version;
					}
					else
					{
						docVersion = getDocVersionFromTmpList(eventParam->subdoc_name);
					}
					WebcfgDebug("docVersion %lu\n", (long) docVersion);
					addWebConfgNotifyMsg(eventParam->subdoc_name, docVersion, "pending", "timer_expired", get_global_transID(),eventParam->timeout, "status", 0);
					WebcfgInfo("retryMultipartSubdoc for EXPIRE case\n");
					rs = retryMultipartSubdoc(eventParam->subdoc_name);
					if(rs == WEBCFG_SUCCESS)
					{
						WebcfgDebug("retryMultipartSubdoc success\n");
					}
					else
					{
						WebcfgError("retryMultipartSubdoc failed\n");
					}
				}
				else if (eventParam->timeout != 0)
				{
					WebcfgInfo("Timeout event. doc apply need time, start timer.\n");
					if( validateEvent(eventParam->subdoc_name, eventParam->trans_id) == WEBCFG_SUCCESS)
					{
						if((getDocVersionFromTmpList(eventParam->subdoc_name))== eventParam->version)
						{
							startWebcfgTimer(eventParam->subdoc_name, eventParam->trans_id, eventParam->timeout);
							addWebConfgNotifyMsg(eventParam->subdoc_name, eventParam->version, "pending", NULL, get_global_transID(),eventParam->timeout, "ack", 0);
						}
						else
						{
							WebcfgError("Timeout event version and tmp cache version are not same\n");
						}
					}
				}
				else
				{
					WebcfgInfo("Crash event. Component restarted after crash, re-send blob.\n");
					uint32_t tmpVersion = 0;
					if(eventParam->version !=0)
					{
						docVersion = eventParam->version;
					}
					else
					{
						docVersion = getDocVersionFromTmpList(eventParam->subdoc_name);
					}
					WebcfgDebug("docVersion is %lu\n", (long)docVersion);

					//If version in event and tmp are not matching, re-send blob to retry.
					if(checkDBVersion(eventParam->subdoc_name, eventParam->version) !=WEBCFG_SUCCESS)
					{
						WebcfgInfo("DB and event version are not same, check tmp list\n");
						tmpVersion = getDocVersionFromTmpList(eventParam->subdoc_name);
						if(tmpVersion != eventParam->version)
						{
							WebcfgInfo("tmp list has new version %lu for doc %s, retry\n", (long)tmpVersion, eventParam->subdoc_name);
							addWebConfgNotifyMsg(eventParam->subdoc_name, docVersion, "failed", "failed_retrying", get_global_transID(),eventParam->timeout,"status",0);
							rs = retryMultipartSubdoc(eventParam->subdoc_name);
							if(rs == WEBCFG_SUCCESS)
							{
								WebcfgDebug("retryMultipartSubdoc success\n");
							}
							else
							{
								WebcfgError("retryMultipartSubdoc failed\n");
							}
						}
						else
						{
							//already in tmp latest version,send success notify, updateDB
							WebcfgInfo("tmp version %lu same as event version %lu\n",(long)tmpVersion, (long)eventParam->version); 
							sendSuccessNotification(eventParam->subdoc_name, eventParam->version, eventParam->trans_id);
							WebcfgInfo("AddToDB subdoc_name %s version %lu\n", eventParam->subdoc_name, (long)eventParam->version);
							checkDBList(eventParam->subdoc_name,eventParam->version);
							WebcfgDebug("checkRootUpdate\n");
							if(checkRootUpdate() == WEBCFG_SUCCESS)
							{
								WebcfgDebug("updateRootVersionToDB\n");
								updateRootVersionToDB();
							}
							addNewDocEntry(get_successDocCount());
						}
					}
					else
					{
						WebcfgInfo("DB and event version are same, check tmp list\n");
						tmpVersion = getDocVersionFromTmpList(eventParam->subdoc_name);
						if(tmpVersion != eventParam->version)
						{
							WebcfgInfo("tmp list has new version %lu for doc %s, retry\n", (long)tmpVersion, eventParam->subdoc_name);
							addWebConfgNotifyMsg(eventParam->subdoc_name, docVersion, "failed", "failed_retrying", get_global_transID(),eventParam->timeout,"status",0);
							//retry with latest tmp version
							rs = retryMultipartSubdoc(eventParam->subdoc_name);
							//wait for ACK to send success notification and Update DB
							if(rs == WEBCFG_SUCCESS)
							{
								WebcfgDebug("retryMultipartSubdoc success\n");
							}
							else
							{
								WebcfgError("retryMultipartSubdoc failed\n");
							}
						}
						else
						{
							WebcfgInfo("Already in latest version, no need to retry\n");
						}
					}
				}
			}
			else
			{
				WebcfgError("Failed to parse event Data\n");
			}
			//WEBCFG_FREE(Data);
		}
		else
		{
			WebcfgDebug("Before pthread cond wait in event consumer thread\n");
			pthread_cond_wait(&event_con, &event_mut);
			pthread_mutex_unlock (&event_mut);
			WebcfgDebug("mutex unlock in event consumer thread after cond wait\n");
		}
	}
	return NULL;
}

//Extract values from comma separated string and add to event_params_t structure.
int parseEventData(char* str, event_params_t **val)
{
	event_params_t *param = NULL;
	char *tmpStr =  NULL;
	char * trans_id = NULL;
	char *version = NULL;
	char * timeout = NULL;
	char *err_code = NULL;

	if(str !=NULL)
	{
		param = (event_params_t *)malloc(sizeof(event_params_t));
		if(param)
		{
			memset(param, 0, sizeof(event_params_t));
		        tmpStr = strdup(str);
			WEBCFG_FREE(str);

		        param->subdoc_name = strsep(&tmpStr, ",");
		        trans_id = strsep(&tmpStr,",");
		        version = strsep(&tmpStr,",");
		        param->status = strsep(&tmpStr,",");
		        timeout = strsep(&tmpStr, ",");

			if(tmpStr !=NULL)
			{
				WebcfgInfo("For NACK event: tmpStr with error_details is %s\n", tmpStr);
				param->process_name = strsep(&tmpStr, ",");
				err_code = strsep(&tmpStr, ",");
				param->failure_reason = strsep(&tmpStr, ",");
				WebcfgInfo("process_name %s err_code %s failure_reason %s\n", param->process_name, err_code, param->failure_reason);
			}

			WebcfgDebug("convert string to uint type\n");
			if(trans_id !=NULL)
			{
				param->trans_id = strtoul(trans_id,NULL,0);
			}

			if(version !=NULL)
			{
				param->version = strtoul(version,NULL,0);
			}

			if(timeout !=NULL)
			{
				param->timeout = strtoul(timeout,NULL,0);
			}

			if(err_code !=NULL)
			{
				param->err_code = strtoul(err_code,NULL,0);
				WebcfgInfo("param->err_code %lu\n", (long)param->err_code);
			}

			WebcfgInfo("param->subdoc_name %s param->trans_id %lu param->version %lu param->status %s param->timeout %lu\n", param->subdoc_name, (long)param->trans_id, (long)param->version, param->status, (long)param->timeout);
			*val = param;
			return WEBCFG_SUCCESS;
		}
	}
	return WEBCFG_FAILURE;

}


//To generate custom timer EXPIRE event for expired doc and add to event queue.
void createTimerExpiryEvent(char *docName, uint16_t transid)
{
	char *expiry_event_data = NULL;
	char data[128] = {0};

	snprintf(data,sizeof(data),"%s,%hu,%u,EXPIRE,%u",docName,transid,0,0);
	expiry_event_data = strdup(data);
	WebcfgInfo("expiry_event_data formed %s\n", expiry_event_data);
	if(expiry_event_data)
	{
		addToEventQueue(expiry_event_data);
		WebcfgDebug("Added EXPIRE event queue\n");
	}
	else
	{
		WebcfgError("Failed to generate timer EXPIRE event\n");
	}
}

//Update Tmp list and send success notification to cloud .
void sendSuccessNotification(char *name, uint32_t version, uint16_t txid)
{
	updateTmpList(name, version, "success", "none", 0, txid, 0);
	addWebConfgNotifyMsg(name, version, "success", "none", get_global_transID(),0, "status",0);
	deleteFromTmpList(name);
}

//start internal timer for required doc when timeout value is received
WEBCFG_STATUS startWebcfgTimer(char *name, uint16_t transID, uint32_t timeout)
{
	if(updateTimerList(true, name, transID, timeout) != WEBCFG_SUCCESS)
	{
		//add docs to timer list
		expire_timer_t *new_node = NULL;
		new_node=(expire_timer_t *)malloc(sizeof(expire_timer_t));

		if(new_node)
		{
			memset( new_node, 0, sizeof( expire_timer_t ) );
			WebcfgDebug("Adding events to list\n");
			new_node->running = true;
			new_node->subdoc_name = strdup(name);
			new_node->txid = transID;
			new_node->timeout = timeout;
			WebcfgDebug("started webcfg internal timer\n");

			new_node->next=NULL;

			if (g_timer_head == NULL)
			{
				g_timer_head = new_node;
			}
			else
			{
				expire_timer_t *temp = NULL;
				WebcfgDebug("Adding next events to list\n");
				temp = g_timer_head;
				while(temp->next !=NULL)
				{
					temp=temp->next;
				}
				temp->next=new_node;
			}

			WebcfgInfo("new_node->subdoc_name %s new_node->txid %lu new_node->timeout %lu status %d added to list\n", new_node->subdoc_name, (long)new_node->txid, (long)new_node->timeout, new_node->running);
			numOfEvents = numOfEvents + 1;
		}
		else
		{
			WebcfgError("Failed in timer allocation\n");
			return WEBCFG_FAILURE;
		}
	}
	WebcfgInfo("startWebcfgTimer success\n");
	return WEBCFG_SUCCESS;
}

//update name, transid, timeout for each doc event
WEBCFG_STATUS updateTimerList(int status, char *docname, uint16_t transid, uint32_t timeout)
{
	expire_timer_t *temp = NULL;
	temp = get_global_timer_node();

	//Traverse through doc list & update required doc timer
	while (NULL != temp)
	{
		WebcfgDebug("node is pointing to temp->subdoc_name %s \n",temp->subdoc_name);
		if( strcmp(docname, temp->subdoc_name) == 0)
		{
			temp->running = status;
			temp->txid = transid;
			temp->timeout = timeout;
			if(strcmp(temp->subdoc_name, docname) !=0)
			{
				WEBCFG_FREE(temp->subdoc_name);
				temp->subdoc_name = NULL;
				temp->subdoc_name = strdup(docname);
			}
			WebcfgInfo("doc timer %s is updated with txid %lu timeout %lu\n", docname, (long)temp->txid, (long)temp->timeout);
			return WEBCFG_SUCCESS;
		}
		temp= temp->next;
	}
	WebcfgDebug("Timer list is empty\n");
	return WEBCFG_FAILURE;
}


//delete doc from webcfg timer list
WEBCFG_STATUS deleteFromTimerList(char* doc_name)
{
	expire_timer_t *prev_node = NULL, *curr_node = NULL;

	if( NULL == doc_name )
	{
		WebcfgError("Invalid value for timer doc\n");
		return WEBCFG_FAILURE;
	}
	WebcfgInfo("timer doc to be deleted: %s\n", doc_name);

	prev_node = NULL;
	curr_node = g_timer_head ;

	// Traverse to get the doc to be deleted
	while( NULL != curr_node )
	{
		if(strcmp(curr_node->subdoc_name, doc_name) == 0)
		{
			WebcfgDebug("Found the node to delete\n");
			if( NULL == prev_node )
			{
				WebcfgDebug("need to delete first doc\n");
				g_timer_head = curr_node->next;
			}
			else
			{
				WebcfgDebug("Traversing to find node\n");
				prev_node->next = curr_node->next;
			}

			WebcfgDebug("Deleting the node entries\n");
			curr_node->running = false;
			curr_node->txid = 0;
			curr_node->timeout = 0;
			WEBCFG_FREE( curr_node->subdoc_name );
			WEBCFG_FREE( curr_node );
			curr_node = NULL;
			WebcfgInfo("Deleted timer successfully and returning..\n");
			numOfEvents =numOfEvents - 1;
			WebcfgInfo("numOfEvents after delete is %d\n", numOfEvents);

			return WEBCFG_SUCCESS;
		}

		prev_node = curr_node;
		curr_node = curr_node->next;
	}
	WebcfgError("Could not find the entry to delete from timer list\n");
	return WEBCFG_FAILURE;
}

//Stop the required doc timer for which ack/nack events received
WEBCFG_STATUS stopWebcfgTimer(char *name, uint16_t trans_id)
{
	expire_timer_t *temp = NULL;
	temp = get_global_timer_node();

	WebcfgInfo("stopWebcfgTimer trans_id %lu\n", (long)trans_id);
	//Traverse through doc list & delete required doc timer from list
	while (NULL != temp)
	{
		WebcfgDebug("node is pointing to temp->subdoc_name %s \n",temp->subdoc_name);
		if( strcmp(name, temp->subdoc_name) == 0)
		{
			if (temp->running)
			{
				if( trans_id ==temp->txid)
				{
					WebcfgInfo("delete timer for sub doc %s\n", name);
					if(deleteFromTimerList(name) == WEBCFG_SUCCESS)
					{
						WebcfgInfo("stopped timer for doc %s\n", name);
						return WEBCFG_SUCCESS;
					}
					else
					{
						WebcfgError("Failed to stop timer for doc %s\n", name);
						break;
					}
				}
				else
				{
					//wait for next event with latest txid T2.
					WebcfgError("temp->txid %lu for doc %s is not matching.\n", (long)temp->txid, name);
					break;
				}
			}
			else
			{
				WebcfgError("timer is not running for doc %s!!!\n", temp->subdoc_name);
				break;
			}
		}
		temp= temp->next;
	}
	return WEBCFG_FAILURE;
}

//check timer expiry by decrementing timeout by 5 for 5s sleep.
int checkTimerExpired (char **exp_doc)
{
	expire_timer_t *temp = NULL;
	temp = get_global_timer_node();

	//Traverse through all docs in list, decrement timer and check if any doc expired.
	while (NULL != temp)
	{
		WebcfgDebug("checking expiry for temp->subdoc_name %s\n",temp->subdoc_name);
		if (temp->running)
		{
			WebcfgInfo("timer running for doc %s temp->timeout: %d\n",temp->subdoc_name, (int)temp->timeout);
			if((int)temp->timeout <= 0)
			{
				WebcfgInfo("Timer Expired for doc %s, doc apply failed\n", temp->subdoc_name);
				*exp_doc = strdup(temp->subdoc_name);
				WebcfgInfo("*exp_doc is %s\n", *exp_doc);
				return true;
			}
			temp->timeout = temp->timeout - 5;
			WebcfgDebug("temp->timeout %d for doc %s\n", temp->timeout, temp->subdoc_name);
		}
		temp= temp->next;
	}
	return false;
}

WEBCFG_STATUS retryMultipartSubdoc(char *docName)
{
	int i =0, m=0;
	WEBCFG_STATUS rv = WEBCFG_FAILURE;
	param_t *reqParam = NULL;
	WDMP_STATUS ret = WDMP_FAILURE;
	int ccspStatus=0;
	int paramCount = 0;
	uint16_t doc_transId = 0;
	webcfgparam_t *pm = NULL;
	multipart_t *gmp = NULL;

	gmp = get_global_mp();

	if(gmp ==NULL)
	{
		WebcfgError("Multipart mp cache is NULL\n");
		return rv;
	}

	if(checkAndUpdateTmpRetryCount(docName) !=WEBCFG_SUCCESS)
	{
		WebcfgError("checkAndUpdateTmpRetryCount failed\n");
		return rv;
	}

	for(m = 0 ; m<((int)gmp->entries_count)-1; m++)
	{
		WebcfgDebug("gmp->entries_count %d\n",(int)gmp->entries_count);
		if(strcmp(gmp->entries[m].name_space, docName) == 0)
		{
			WebcfgDebug("gmp->entries[%d].name_space %s\n", m, gmp->entries[m].name_space);
			WebcfgDebug("gmp->entries[%d].etag %lu\n" ,m,  (long)gmp->entries[m].etag);
			WebcfgDebug("gmp->entries[%d].data %s\n" ,m,  gmp->entries[m].data);
			WebcfgDebug("gmp->entries[%d].data_size is %zu\n", m,gmp->entries[m].data_size);

			WebcfgDebug("--------------decode root doc-------------\n");
			pm = webcfgparam_convert( gmp->entries[m].data, gmp->entries[m].data_size+1 );
			if ( NULL != pm)
			{
				paramCount = (int)pm->entries_count;

				reqParam = (param_t *) malloc(sizeof(param_t) * paramCount);
				memset(reqParam,0,(sizeof(param_t) * paramCount));

				WebcfgDebug("paramCount is %d\n", paramCount);
				for (i = 0; i < paramCount; i++)
				{
			                if(pm->entries[i].value != NULL)
			                {
						if(pm->entries[i].type == WDMP_BLOB)
						{
							char *appended_doc = NULL;
							WebcfgDebug("B4 webcfg_appendeddoc\n");
							appended_doc = webcfg_appendeddoc( gmp->entries[m].name_space, gmp->entries[m].etag, pm->entries[i].value, pm->entries[i].value_size, &doc_transId);
							WebcfgInfo("webcfg_appendeddoc doc_transId is %hu\n", doc_transId);
							reqParam[i].name = strdup(pm->entries[i].name);
							WebcfgInfo("appended_doc length: %zu\n", strlen(appended_doc));
							reqParam[i].value = strdup(appended_doc);
							reqParam[i].type = WDMP_BASE64;
							WEBCFG_FREE(appended_doc);
							WebcfgDebug("appended_doc done\n");
							WebcfgInfo("Update doc trans_id\n");
							//update doc_transId only for blob docs, not for scalars.
							updateTmpList(gmp->entries[m].name_space, gmp->entries[m].etag, "pending", "failed_retrying", ccspStatus, doc_transId, 1);
						}
						else
						{
							WebcfgError("blob type is incorrect\n");
						}
			                }
					WebcfgInfo("Request:> param[%d].name = %s, type = %d\n",i,reqParam[i].name,reqParam[i].type);
					WebcfgDebug("Request:> param[%d].value = %s\n",i,reqParam[i].value);
				}

				WebcfgDebug("Proceed to setValues..\n");
				if(reqParam !=NULL)
				{
					WebcfgInfo("retryMultipartSubdoc WebConfig SET Request\n");
					setValues(reqParam, paramCount, ATOMIC_SET_WEBCONFIG, NULL, NULL, &ret, &ccspStatus);
					if(ret == WDMP_SUCCESS)
					{
						WebcfgInfo("retryMultipartSubdoc setValues success. ccspStatus : %d\n", ccspStatus);
						if(reqParam[0].type  != WDMP_BASE64)
						{
							WebcfgInfo("For scalar docs, update trans_id as 0\n");
							updateTmpList(gmp->entries[m].name_space, gmp->entries[m].etag, "success", "none", 0, 0, 0);
							//send scalar success notification, delete tmp, updateDB
							addWebConfgNotifyMsg(gmp->entries[m].name_space, gmp->entries[m].etag, "success", "none", get_global_transID(), 0, "status", 0);
							WebcfgInfo("deleteFromTmpList as scalar doc is applied\n");
							deleteFromTmpList(gmp->entries[m].name_space);
							checkDBList(gmp->entries[m].name_space,gmp->entries[m].etag);
							WebcfgInfo("checkRootUpdate scalar doc case\n");
							if(checkRootUpdate() == WEBCFG_SUCCESS)
							{
								WebcfgInfo("updateRootVersionToDB\n");
								updateRootVersionToDB();
							}
							addNewDocEntry(get_successDocCount());
						}
						rv = WEBCFG_SUCCESS;
					}
					else
					{
						WebcfgError("retryMultipartSubdoc setValues Failed. ccspStatus : %d\n", ccspStatus);
						if((ccspStatus == 192) || (ccspStatus == 204) || (ccspStatus == 191))
						{
							WebcfgInfo("ccspStatus is crash %d\n", ccspStatus);
							updateTmpList(gmp->entries[m].name_space, gmp->entries[m].etag, "pending", "failed_retrying", ccspStatus, 0, 1);
							addWebConfgNotifyMsg(gmp->entries[m].name_space, gmp->entries[m].etag, "pending", "failed_retrying", get_global_transID(), 0,"status",ccspStatus);
							set_doc_fail(1);
							WebcfgInfo("the retry flag value is %d\n", get_doc_fail());
						}
						else
						{
							updateTmpList(gmp->entries[m].name_space, gmp->entries[m].etag, "failed", "doc_rejected", ccspStatus, 0, 0);
							addWebConfgNotifyMsg(gmp->entries[m].name_space, gmp->entries[m].etag, "failed", "doc_rejected", get_global_transID(), 0, "status", ccspStatus);
						}
					}
					reqParam_destroy(paramCount, reqParam);
				}
				webcfgparam_destroy( pm );
			}
			else
			{
				WebcfgError("--------------decode root doc failed-------------\n");
			}
			break;
		}
		else
		{
			WebcfgDebug("docName %s not found in mp list\n", docName);
		}
	}
	return rv;
}

WEBCFG_STATUS checkDBVersion(char *docname, uint32_t version)
{
	webconfig_db_data_t *webcfgdb = NULL;
	webcfgdb = get_global_db_node();

	//Traverse through doc list & check version for required doc
	while (NULL != webcfgdb)
	{
		WebcfgDebug("node is pointing to webcfgdb->name %s, docname %s, webcfgdb->version %lu, version %lu \n",webcfgdb->name, docname, (long)webcfgdb->version, (long)version);
		if( strcmp(docname, webcfgdb->name) == 0)
		{
			if(webcfgdb->version == version)
			{
				WebcfgInfo("webcfgdb version %lu is same for doc %s\n", (long)webcfgdb->version, docname);
				return WEBCFG_SUCCESS;
			}
		}
		webcfgdb= webcfgdb->next;
	}
	return WEBCFG_FAILURE;
}

WEBCFG_STATUS checkAndUpdateTmpRetryCount(char *docname)
{
	webconfig_tmp_data_t *temp = NULL;
	temp = get_global_tmp_node();

	//Traverse through doc list & check retry count reached max attempt.
	while (NULL != temp)
	{
		WebcfgDebug("checkAndUpdateTmpRetryCount: temp->name %s, temp->version %lu, temp->retry_count %d\n",temp->name, (long)temp->version, temp->retry_count);
		if( strcmp(docname, temp->name) == 0)
		{
			if(temp->retry_count >= MAX_APPLY_RETRY_COUNT)
			{
				WebcfgInfo("Apply retry_count %d has reached max limit for doc %s\n", temp->retry_count, docname);
				//send max retry notification to cloud.
				addWebConfgNotifyMsg(temp->name, temp->version, "failed", "max_retry_reached", get_global_transID(),0,"status",0);
				return WEBCFG_FAILURE;
			}
			temp->retry_count++;
			WebcfgInfo("temp->retry_count updated to %d for docname %s\n",temp->retry_count, docname);
			return WEBCFG_SUCCESS;
		}
		temp= temp->next;
	}
	return WEBCFG_FAILURE;
}

uint32_t getDocVersionFromTmpList(char *docname)
{
	webconfig_tmp_data_t *temp = NULL;
	temp = get_global_tmp_node();

	//Traverse through doc list & fetch version for required doc.
	while (NULL != temp)
	{
		WebcfgDebug("getDocVersionFromTmpList: temp->name %s, temp->version %lu\n",temp->name, (long)temp->version);
		if( strcmp(docname, temp->name) == 0)
		{
			WebcfgInfo("return temp->version %lu for docname %s\n", (long)temp->version, docname);
			return temp->version;
		}
		temp= temp->next;
	}
	return 0;
}

//validate each event based on exact doc txid in tmp list.
WEBCFG_STATUS validateEvent(char *docname, uint16_t txid)
{
	webconfig_tmp_data_t *temp = NULL;
	temp = get_global_tmp_node();

	//Traverse through doc list & fetch version for required doc.
	while (NULL != temp)
	{
		WebcfgDebug("validateEvent: temp->name %s, temp->trans_id %hu\n",temp->name, temp->trans_id);
		if( strcmp(docname, temp->name) == 0)
		{
			if(txid == temp->trans_id)
			{
				WebcfgInfo("Valid event. Event txid %hu matches with temp trans_id %hu\n", txid, temp->trans_id);
				return WEBCFG_SUCCESS;
			}
			else
			{
				WebcfgError("Not a valid event. Event txid %hu does not match with temp trans_id %hu\n", txid, temp->trans_id);
				break;
			}
		}
		temp= temp->next;
	}
	return WEBCFG_FAILURE;
}
