/*
*    
*    Copyright (C) 2012  Stephen A. Rodgers
*
*    This program is free software: you can redistribute it and/or modify
*    it under the terms of the GNU General Public License as published by
*    the Free Software Foundation, either version 3 of the License, or
*    (at your option) any later version.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU General Public License for more details.
*
*    You should have received a copy of the GNU General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*
*    
*
*
*/


/* Define these if not defined */

#ifndef VERSION
	#define VERSION "X.X.X"
#endif

#ifndef EMAIL
	#define EMAIL "hwstar@rodgers.sdcoxmail.com"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <signal.h>
#include <ctype.h>
#include <getopt.h>
#include <limits.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <poll.h>
#include <xPL.h>
#include "types.h"
#include "notify.h"
#include "confread.h"
#include "socket.h"

#define MALLOC_ERROR	malloc_error(__FILE__,__LINE__)

#define SHORT_OPTIONS "c:d:f:hi:l:ns:v"

#define WS_SIZE 256

#define DEF_PID_FILE		"/var/run/xplhan.pid"
#define DEF_CONFIG_FILE		"/etc/xplhan.conf"
#define DEF_INSTANCE_ID		"test"
#define DEF_HOST			"localhost"
#define DEF_SERVICE			"1129"

#define MAX_SERVICES 64
#define MAX_CHANNEL 16
#define MAX_UNITS_PER_COMMAND 5
#define MAX_HAN_DEVICE 16
#define MAX_POLL_INTERVAL 604800

typedef enum {GNOP=0x00, GVLV= 0x10, GRLY= 0x11, GTMP=0x12, GOUT=0x13, GINP=0x14, GACD=0x15, GVLT=0x16, 
               GCUR=0x17,GHUM= 0x30, GWSP= 0x31, GWDR = 0x32, GRGC = 0x33} hanCommands_t;

typedef enum {NULLUNIT=0, FAHRENHEIT, CELSIUS, VOLTS, AMPS, HERTZ, OUTPUT, PERCENTRH, MPH, KMH, _WDIRMAP, IN, MM} units_t;
 
typedef struct cloverrides {
	unsigned pid_file : 1;
	unsigned instance_id : 1;
	unsigned log_path : 1;
	unsigned interface : 1;
} clOverride_t;

/* Command keyword to code map */
typedef struct han_command_map hanCommandMap_t;
struct han_command_map{
	hanCommands_t code;
	units_t valid_units[MAX_UNITS_PER_COMMAND];
	String keyword;
};

/* Units to code map */
typedef struct units_map unitsMap_t;
struct units_map{
	units_t code;
	String keyword;
};

	
typedef struct service_entry serviceEntry_t;
typedef serviceEntry_t * serviceEntryPtr_t;

/*
 * Service entry data structure.
 * There is one of these for each virtual service in this gateway
 */
 
struct service_entry
{
	Bool is_sensor;
	int poll_last;
	unsigned address;
	unsigned polling_interval;
	unsigned poll_counter;
	unsigned service_id;
	unsigned channel;
	units_t units;
	char *units_keyword;
	uint32_t iid_hash;
	hanCommands_t cmd;
	float poll_f_last;
	String instance_id;
	String class;
	String type;
	xPL_ServicePtr xplService;
	serviceEntryPtr_t prev;
	serviceEntryPtr_t next;		
};

/*
 * Work Queue data structure
 * These are created by incoming xPL requests and commands
 */


typedef struct workq_entry workQEntry_t;
typedef workQEntry_t * workQEntryPtr_t;


struct workq_entry
{
	Bool is_poll;
	serviceEntryPtr_t sp;
	String cmd;
	workQEntryPtr_t prev;
	workQEntryPtr_t next;
};


typedef struct response response_t;
typedef response_t * responsePtr_t;

struct response
{
	uint_least8_t address;
	uint_least8_t command;
	uint_least8_t params[16];
}__attribute__ ((__packed__));
	

/*
 * Variables
 */

char *progName;
int debugLvl = 0; 

static Bool noBackground = FALSE;
static Bool cmdFail = FALSE;
static int hanSock = -1;
static clOverride_t clOverride = {0,0,0,0};

static serviceEntryPtr_t serviceEntryHead = NULL;
static serviceEntryPtr_t serviceEntryTail = NULL;
static workQEntryPtr_t pendingResponse = NULL;
static workQEntryPtr_t workQHead = NULL;
static workQEntryPtr_t workQTail = NULL;

static ConfigEntryPtr_t	configEntry = NULL;

static char configFile[WS_SIZE] = DEF_CONFIG_FILE;
static char interface[WS_SIZE] = "";
static char logPath[WS_SIZE] = "";
static char instanceID[WS_SIZE] = DEF_INSTANCE_ID;
static char pidFile[WS_SIZE] = DEF_PID_FILE;
static char host[WS_SIZE] = DEF_HOST;
static char service[WS_SIZE] = DEF_SERVICE;


/* Commandline options. */

static const struct option longOptions[] = {
	{"config-file", 1, 0, 'c'},
	{"debug", 1, 0, 'd'},
	{"help", 0, 0, 'h'},
	{"interface", 1, 0, 'i'},	
	{"instance", 1, 0, 's'},	
	{"log", 1, 0, 'l'},
	{"no-background", 0, 0, 'n'},
	{"pid-file", 0, 0, 'f'},
	{"version", 0, 0, 'v'},
	{0, 0, 0, 0}
};

/* Han command map */

static const hanCommandMap_t hanCommandMap[] = {
	{GTMP, {FAHRENHEIT, CELSIUS, NULLUNIT}, "gtmp"},
	{GACD, {VOLTS, HERTZ, NULLUNIT}, "gacd"},
	{GOUT, {OUTPUT,NULLUNIT}, "gout"},
	{GVLT, {VOLTS,NULLUNIT}, "gvlt"},
	{GCUR, {AMPS,NULLUNIT}, "gcur"},
	{GHUM, {PERCENTRH,NULLUNIT},"ghum"},
	{GWSP, {MPH,KMH,NULLUNIT},"gwsp"},
	{GWDR, {_WDIRMAP,NULLUNIT},"gwdr"},
	{GRGC, {IN,MM,NULLUNIT},"grgc"},
	{GNOP, {NULLUNIT}, NULL}
};

/* Units map */

static const unitsMap_t unitsMap[] = {
	{FAHRENHEIT, "fahrenheit"},
	{CELSIUS,"celsius"},
	{VOLTS,"volts"},
	{AMPS,"amps"},
	{HERTZ,"hertz"},
	{OUTPUT,"output"},
	{PERCENTRH,"%rh"},
	{MPH,"mph"},
	{KMH,"kmh"},
	{_WDIRMAP,"wdirmap"},
	{IN,"in."},
	{MM,"mm."},
	{NULLUNIT, NULL}
};

static char *dirmap[16] = {
	"ese",
	"ene",
	"e",
	"sse",
	"se",
	"ssw",
	"s",
	"nne",
	"ne",
	"wsw",
	"sw",
	"nnw",
	"n",
	"wnw",
	"nw",
	"w" };
	
/* 
 * Allocate a memory block and zero it out
 */

static void *mallocz(size_t size)
{
	void *m = calloc(size, sizeof(uint8_t));
	return m;
}
 
/*
 * Malloc error handler
 */
 
static void malloc_error(String file, int line)
{
	fatal("Out of memory in file %s, at line %d");
}


/*
* Convert a string to an unsigned int with bounds checking
*/

static Bool str2uns(String s, unsigned *num, unsigned min, unsigned max)
{
		long val;
		int len,i;
		if((!num) || (!s)){
			debug(DEBUG_UNEXPECTED, "NULL pointer passed to str2uns");
			return FALSE;
		}
		
		len = strlen(s);
		
		for(i = 0; i < len; i++){
			if(!isdigit(s[i]))
				break;
		}
		if(i != len)
			return FALSE;
			
		val = strtol(s, NULL, 0);
		if((val < min) || (val > max))
			return FALSE;
		*num = (unsigned) val;
		return TRUE;
}


/*
* Duplicate or split a string. 
*
* The string is copied, and the sep characters are replaced with nul's and a list pointers
* is built. 
* 
* If no sep characters are found, the string is just duped and returned.
*
* This function returns the number of arguments found.
*
* When the caller is finished with the list and the return value is non-zero he should free() the first entry.
* 
*
*/

static int dupOrSplitString(const String src, String *list, char sep, int limit)
{
		String p, q, srcCopy;
		int i;
		

		if((!src) || (!list) || (!limit))
			return 0;

		if(!(srcCopy = strdup(src)))
			MALLOC_ERROR;

		for(i = 0, q = srcCopy; (i < limit) && (p = strchr(q, sep)); i++, q = p + 1){
			*p = 0;
			list[i] = q;
		
		}

		list[i] = q;
		i++;

		return i;
}

/* 
 * Get the pid from a pidfile.  Returns the pid or -1 if it couldn't get the
 * pid (either not there, stale, or not accesible).
 */
static pid_t pid_read(char *filename) {
	FILE *file;
	pid_t pid;
	
	/* Get the pid from the file. */
	file=fopen(filename, "r");
	if(!file) {
		return(-1);
	}
	if(fscanf(file, "%d", &pid) != 1) {
		fclose(file);
		return(-1);
	}
	if(fclose(file) != 0) {
		return(-1);
	}
	
	/* Check that a process is running on this pid. */
	if(kill(pid, 0) != 0) {
		
		/* It might just be bad permissions, check to be sure. */
		if(errno == ESRCH) {
			return(-1);
		}
	}
	
	/* Return this pid. */
	return(pid);
}


/* 
 * Write the pid into a pid file.  Returns zero if it worked, non-zero
 * otherwise.
 */
static int pid_write(char *filename, pid_t pid) {
	FILE *file;
	
	/* Create the file. */
	file=fopen(filename, "w");
	if(!file) {
		return -1;
	}
	
	/* Write the pid into the file. */
	(void) fprintf(file, "%d\n", pid);
	if(ferror(file) != 0) {
		(void) fclose(file);
		return -1;
	}
	
	/* Close the file. */
	if(fclose(file) != 0) {
		return -1;
	}
	
	/* We finished ok. */
	return 0;
}

/*
* When the user hits ^C, logically shutdown
* (including telling the network the service is ending)
*/

static void shutdownHandler(int onSignal)
{
	serviceEntryPtr_t sp;
	
	for(sp = serviceEntryTail; sp; sp = sp->prev){
		xPL_setServiceEnabled(sp->xplService, FALSE);
		xPL_releaseService(sp->xplService);
		xPL_shutdown();
	}
	/* Unlink the pid file if we can. */
	(void) unlink(pidFile);
	exit(0);
}

/* 
 * Dequeue work Queue Entry
 */
  
workQEntryPtr_t dequeueWorkQueueEntry()
{
	workQEntryPtr_t res = NULL;
	
	if(workQTail){
		res = workQTail;
		if(workQTail->prev)
			workQTail->prev->next = NULL;
		else
			workQHead = NULL;
		workQTail = workQTail->prev;
	}
	return res;	
}

/* 
 * Free a work queue entry
 */

void freeWorkQueueEntry(workQEntryPtr_t wqe)
{
	if(wqe){
		if(wqe->cmd)
			free(wqe->cmd);
		free(wqe);
	}
}

/* 
 * Add a command to the work queue
 */
 
void queueCommand(String cmd, serviceEntryPtr_t sp, Bool isPoll)
{

	workQEntryPtr_t wq = NULL;
	
	/* Allocate work queue entry */
	if(!(wq = mallocz(sizeof(workQEntry_t))))
		MALLOC_ERROR;
	debug(DEBUG_ACTION, "queueCommand()");
	wq->cmd = cmd;
	wq->is_poll = isPoll;
	wq->sp = sp;
	
	if(!workQHead)
		workQHead = workQTail = wq;
	else{
		wq->next = workQHead;
		workQHead->prev = wq;
		workQHead = wq;
	}
}

/*
 * Act on the response from a GOUT command
 */
 
void GOUTAction(unsigned char pcount, responsePtr_t resp, workQEntryPtr_t wq)
{
	int msgType = xPL_MESSAGE_STATUS;
	char *res, dev[12];
	xPL_MessagePtr msg = NULL;
	serviceEntryPtr_t sp = NULL;
	


	if((!resp) ||(!wq) || (!wq->sp))
		return;
		
	sp = wq->sp;
	
	/* Check for correct number of parameters */
	
	if(pcount != 3){
		debug(DEBUG_UNEXPECTED, "GOUTAction(): Received an incorrect number of parameters, got %u, need 3", pcount);
		return;
	}

	if(resp->params[1] != 2)
		return; /* Only respond when status is requested */
		
	/* Conversion statements */
	if(resp->params[2] == 1)
		res = "high";
	else if(resp->params[2] == 0)
		res = "low";
	else{
		debug(DEBUG_UNEXPECTED,"Unexpected state received: %d", resp->params[2]);
		return; 
	}
	
	/* Format device as string */
	snprintf(dev, 12, "%d", resp->params[0]);

	/* Build a message */
	
	if(wq->is_poll){ /* Was this the result of a poll */
		if(resp->params[2] == sp->poll_last) /* Was there a change ? */
			return;
		debug(DEBUG_EXPECTED, "Sending trigger");
		sp->poll_last = resp->params[2];
		msgType = xPL_MESSAGE_TRIGGER;
	}
	
	
	if(!(msg = xPL_createBroadcastMessage(sp->xplService, msgType)))
		debug(DEBUG_UNEXPECTED, "GOUTaction(): Could not create status message");
	xPL_setSchema(msg, "sensor", "basic");
	xPL_setMessageNamedValue(msg, "device", dev);
	xPL_setMessageNamedValue(msg, "type", "output");
	xPL_setMessageNamedValue(msg, "current", res);
	
	
	/* Send the message */
	
	xPL_sendMessage(msg);
	
	/* Release the resource */
	
	xPL_releaseMessage(msg);

}


/*
 * Act on the response from a GACD command
 */
 
void GACDAction(unsigned char pcount, responsePtr_t resp, workQEntryPtr_t wq)
{
	char ws[12];
	int msgType = xPL_MESSAGE_STATUS;
	uint_least16_t voltsX10, freqX100;
	xPL_MessagePtr msg = NULL;
	float volts, freq;
	serviceEntryPtr_t sp = NULL;

	if((!resp) ||(!wq) || (!wq->sp))
		return;
		
	sp = wq->sp;
	
	
	/* Check for correct number of parameters */
	
	if(pcount != 4){
		debug(DEBUG_UNEXPECTED, "GACDAction(): Received an incorrect number of parameters, got %u, need 4", pcount);
		return;
	}
	/* Conversion statements */
	
	voltsX10 = (((uint_least16_t) resp->params[1]) << 8) + resp->params[0];
	freqX100 = (((uint_least16_t) resp->params[3]) << 8) + resp->params[2];
	volts = ((float) voltsX10)/10;
	freq = ((float) freqX100)/100;
	
	debug(DEBUG_ACTION, "AC Volts = %3.1f, AC Frequency = %2.2f", volts, freq);
	
	/* Build a message */
	
	
	if(wq->is_poll){ /* Was this the result of a poll */
		float thisMeas = (sp->units == VOLTS) ? volts : freq;
		if(sp->poll_f_last == thisMeas) /* Was there a change ? */
			return;
		debug(DEBUG_EXPECTED, "Sending trigger");
		sp->poll_f_last = thisMeas;
		msgType = xPL_MESSAGE_TRIGGER;
	}

	
	if(!(msg = xPL_createBroadcastMessage(sp->xplService, msgType)))
		debug(DEBUG_UNEXPECTED, "gtmpaction(): Could not create status message");
	xPL_setSchema(msg, "sensor", "basic");
	if(sp->units == VOLTS){ /* Voltage or frequency? */
		xPL_setMessageNamedValue(msg, "type", "volts");
		snprintf(ws, 10, "%3.1f", volts);
		xPL_setMessageNamedValue(msg, "current", ws);
		xPL_setMessageNamedValue(msg, "units", "volts");
	}
	else{
		xPL_setMessageNamedValue(msg, "type", "frequency");
		snprintf(ws, 10, "%2.2f", freq);
		xPL_setMessageNamedValue(msg, "current", ws);
		xPL_setMessageNamedValue(msg, "units", "hertz");
	}
	
	/* Send the message */
	
	xPL_sendMessage(msg);
	
	/* Release the resource */
	
	xPL_releaseMessage(msg);

}




/*
 * Act on response from GTMP command
 */

void GTMPAction(unsigned char pcount, responsePtr_t resp, workQEntryPtr_t wq)
{
	int val = 0;
	int msgType = xPL_MESSAGE_STATUS;
	char ws[12];
	unsigned countsPerC;
	int_least16_t rawTemp;
	xPL_MessagePtr msg = NULL;
	serviceEntryPtr_t sp = NULL;
	
	if((!resp) ||(!wq) || (!wq->sp))
		return;
		
	sp = wq->sp;
	
	if(pcount != 5){
		debug(DEBUG_UNEXPECTED, "GTMPAction(): Received an incorrect number of parameters, got %u, need 5", pcount);
		return;
	}
	countsPerC = (unsigned) resp->params[1];
	rawTemp = (int_least16_t) ((((uint_least16_t) resp->params[4]) << 8) + resp->params[3]);
	
	printf("Raw temp = %d, counts per c = %d\n", rawTemp, countsPerC);
	
	/* Do conversion per units field */
	if(sp->units == CELSIUS){
		val = rawTemp / countsPerC;
	}
	else if (sp->units == FAHRENHEIT){
		val = (9 * ((int) rawTemp))/(5 * ((int) countsPerC)) + 32;
	}
	else{
		debug(DEBUG_UNEXPECTED, "GTMPAction(): Invalid unit for conversion");
	}


	
	if(wq->is_poll){ /* Was this the result of a poll */
		if(val == sp->poll_last) /* Was there a change ? */
			return;
		debug(DEBUG_EXPECTED, "Sending trigger");
		sp->poll_last = val;
		msgType = xPL_MESSAGE_TRIGGER;
	}
	

	
	if(!(msg = xPL_createBroadcastMessage(sp->xplService, msgType)))
		debug(DEBUG_UNEXPECTED, "GTMPaction(): Could not create message");
	xPL_setSchema(msg,"sensor","basic");
	xPL_setMessageNamedValue(msg, "device", "0"); 
	xPL_setMessageNamedValue(msg, "type", "temp");
	snprintf(ws, 10, "%d", val);
	xPL_setMessageNamedValue(msg, "current", ws);
	xPL_setMessageNamedValue(msg, "units", (sp->units == CELSIUS) ? "celsius" : "fahrenheit");
	xPL_sendMessage(msg);
	xPL_releaseMessage(msg);

}

/*
 * Act on the response from a GVLT command
 */
 

void GVLTAction(unsigned char pcount, responsePtr_t resp, workQEntryPtr_t wq)
{
	int msgType = xPL_MESSAGE_STATUS;
	xPL_MessagePtr msg = NULL;
	serviceEntryPtr_t sp = NULL;
	uint32_t voltres;
	int8_t voltexp;
	uint16_t rawvolts;
	float voltage;
	char ws[12];
	
	if((!resp) ||(!wq) || (!wq->sp))
		return;
		
	sp = wq->sp;
	
	if(pcount != 8){
		debug(DEBUG_UNEXPECTED, "GVLTAction(): Received an incorrect number of parameters, got %u, need 8", pcount);
		return;
	}
	
	/* Extract values */
	
	voltres = ((uint32_t) resp->params[4]) + 
			  (((uint32_t) resp->params[5]) * 256 ) +
			  (((uint32_t) resp->params[6]) * 65536) +
			  (((uint32_t) resp->params[7]) * 16777216);

			  
	voltexp = (int8_t) resp->params[1];
	rawvolts = (uint16_t) ((((uint16_t) resp->params[3]) * 256) + resp->params[2]);
	
	debug(DEBUG_ACTION, "GVLTAction(): voltres = %u", voltres);
	debug(DEBUG_ACTION, "GVLTAction(): voltexp = %d", voltexp);
	debug(DEBUG_ACTION, "GVLTAction(): rawvolts = %u", rawvolts);
	
	/* Scale result */

	voltage = ((float) rawvolts) * ((float)voltres)*powf(10.0,voltexp);
	debug(DEBUG_ACTION, "GVLTAction(): Voltage = %3.3f", voltage);


	/* Test for change */
	
	if(wq->is_poll){ /* Was this the result of a poll */
		if(voltage == sp->poll_f_last) /* Was there a change ? */
			return;
		debug(DEBUG_EXPECTED, "Sending trigger");
		sp->poll_f_last = voltage;
		msgType = xPL_MESSAGE_TRIGGER;
	}
	
    /* Send message */
    
	if(!(msg = xPL_createBroadcastMessage(sp->xplService, msgType))){
		debug(DEBUG_UNEXPECTED, "GVLTaction(): Could not create message");
		return;
	}
	xPL_setSchema(msg,"sensor","basic");
	xPL_setMessageNamedValue(msg, "device", "0"); 
	xPL_setMessageNamedValue(msg, "type", "volts");
	snprintf(ws, 10, "%3.3f", voltage);
	xPL_setMessageNamedValue(msg, "current", ws);
	xPL_setMessageNamedValue(msg, "units", "volts");
	xPL_sendMessage(msg);
	xPL_releaseMessage(msg);
}

/*
 * Act on the response from a GCUR command
 */
 

void GCURAction(unsigned char pcount, responsePtr_t resp, workQEntryPtr_t wq)
{
	int msgType = xPL_MESSAGE_STATUS;
	xPL_MessagePtr msg = NULL;
	serviceEntryPtr_t sp = NULL;
	uint32_t ampsres;
	int8_t ampsexp;
	int16_t rawamps;
	float amps;
	char ws[12];
	union {
		int16_t amps16;
		uint8_t amps8[2];
	}sconv;
	
	if((!resp) ||(!wq) || (!wq->sp))
		return;
		
	sp = wq->sp;
	
	if(pcount != 8){
		debug(DEBUG_UNEXPECTED, "GCURAction(): Received an incorrect number of parameters, got %u, need 8", pcount);
		return;
	}
	
	/* Extract values */
	
	ampsres = ((uint32_t) resp->params[4]) + 
			  (((uint32_t) resp->params[5]) * 256 ) +
			  (((uint32_t) resp->params[6]) * 65536) +
			  (((uint32_t) resp->params[7]) * 16777216);

			  
	ampsexp = (int8_t) resp->params[1];
	
	#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
		sconv.amps8[0] = resp->params[2];
		sconv.amps8[1] = resp->params[3];
	#else
		sconv.amps8[1] = resp->params[2];
		sconv.amps8[0] = resp->params[3];
	#endif	
	
	rawamps = sconv.amps16;
	
	
	debug(DEBUG_ACTION, "GCURAction(): ampsres = %u", ampsres);
	debug(DEBUG_ACTION, "GCURAction(): ampsexp = %d", ampsexp);
	debug(DEBUG_ACTION, "GCURAction(): rawamps = %d", rawamps);
	
	/* Scale result */

	amps = ((float) rawamps) * ((float)ampsres)*powf(10.0,ampsexp);
	debug(DEBUG_ACTION, "GCURAction(): Amps = %3.3f", amps);


	/* Test for change */
	
	if(wq->is_poll){ /* Was this the result of a poll */
		if(amps == sp->poll_f_last) /* Was there a change ? */
			return;
		debug(DEBUG_EXPECTED, "Sending trigger");
		sp->poll_f_last = amps;
		msgType = xPL_MESSAGE_TRIGGER;
	}
	
    /* Send message */
    
	if(!(msg = xPL_createBroadcastMessage(sp->xplService, msgType))){
		debug(DEBUG_UNEXPECTED, "GCURaction(): Could not create message");
		return;
	}
	xPL_setSchema(msg,"sensor","basic");
	xPL_setMessageNamedValue(msg, "device", "0"); 
	xPL_setMessageNamedValue(msg, "type", "amps");
	snprintf(ws, 10, "%3.3f", amps);
	xPL_setMessageNamedValue(msg, "current", ws);
	xPL_setMessageNamedValue(msg, "units", "amps");
	xPL_sendMessage(msg);
	xPL_releaseMessage(msg);
}

/*
 * Act on response from GHUM command
 */

void GHUMAction(unsigned char pcount, responsePtr_t resp, workQEntryPtr_t wq)
{
	float val = 0;
	int msgType = xPL_MESSAGE_STATUS;
	char ws[12];
	unsigned countsPerRHP;
	int_least16_t rawHum;
	xPL_MessagePtr msg = NULL;
	serviceEntryPtr_t sp = NULL;
	
	if((!resp) ||(!wq) || (!wq->sp))
		return;
		
	sp = wq->sp;
	
	if(pcount != 6){
		debug(DEBUG_UNEXPECTED, "GHUMAction(): Received an incorrect number of parameters, got %u, need 6", pcount);
		return;
	}
	countsPerRHP = (unsigned) resp->params[1];
	rawHum = (int_least16_t) ((((uint_least16_t) resp->params[4]) << 8) + resp->params[3]);
	
	if(resp->params[5])
		debug(DEBUG_UNEXPECTED, "GHUMAction():  Sensor error: code = %d", resp->params[5]);
	else
		debug(DEBUG_EXPECTED, "Raw humidity = %d, counts per c = %d\n", rawHum, countsPerRHP);
	
	/* Do conversion per units field */

	if (sp->units == PERCENTRH){
		val = ((float) rawHum)/((int) countsPerRHP);
	}
	else{
		debug(DEBUG_UNEXPECTED, "GHUMAction(): Invalid unit for conversion");
	}


	
	if(wq->is_poll){ /* Was this the result of a poll */
		if(val == sp->poll_f_last) /* Was there a change ? */
			return;
		debug(DEBUG_EXPECTED, "Sending trigger");
		sp->poll_f_last = val;
		msgType = xPL_MESSAGE_TRIGGER;
	}
	

	
	if(!(msg = xPL_createBroadcastMessage(sp->xplService, msgType)))
		debug(DEBUG_UNEXPECTED, "GHUMaction(): Could not create message");
	xPL_setSchema(msg,"sensor","basic");
	xPL_setMessageNamedValue(msg, "device", "0"); 
	xPL_setMessageNamedValue(msg, "type", "humidity");
	snprintf(ws, 10, "%2.1f", val);
	xPL_setMessageNamedValue(msg, "current", ws);
	xPL_setMessageNamedValue(msg, "units", "%rh");
	xPL_sendMessage(msg);
	xPL_releaseMessage(msg);

}

/*
 * Act on response from GWSP command
 */

void GWSPAction(unsigned char pcount, responsePtr_t resp, workQEntryPtr_t wq)
{
	float val;
	int msgType = xPL_MESSAGE_STATUS;
	char ws[12];
	uint_least32_t counts;
	uint_least16_t mantissa;
	int_least8_t exponent;
	xPL_MessagePtr msg = NULL;
	serviceEntryPtr_t sp = NULL;
	
	if((!resp) ||(!wq) || (!wq->sp))
		return;
		
	sp = wq->sp;
	
	if(pcount != 6){
		debug(DEBUG_UNEXPECTED, "GWSPAction(): Received an incorrect number of parameters, got %u, need 6", pcount);
		return;
	}
	exponent = (int_least8_t) resp->params[1];
	mantissa =  ((((uint_least16_t) resp->params[3]) << 8) + resp->params[2]);
	counts =  ((((uint_least32_t) resp->params[5]) << 8) + resp->params[4]);
	
	printf("Raw counts  = %u", counts);
	
	/* Calculate result */
	val =  ((float)mantissa)*powf(10.0,exponent) / (float) counts;
	debug(DEBUG_ACTION, "GWSPAction(): Windspeed = %3.1f kmh", val);
	
	
	
	/* Do conversion per units field */

	if (sp->units == KMH);
	else if(sp->units == MPH)
		val *= 0.621371;
	else{
		debug(DEBUG_UNEXPECTED, "GWSPAction(): Invalid unit for conversion");
	}


	
	if(wq->is_poll){ /* Was this the result of a poll */
		if(val == sp->poll_f_last) /* Was there a change ? */
			return;
		debug(DEBUG_EXPECTED, "Sending trigger");
		sp->poll_f_last = val;
		msgType = xPL_MESSAGE_TRIGGER;
	}
	

	
	if(!(msg = xPL_createBroadcastMessage(sp->xplService, msgType)))
		debug(DEBUG_UNEXPECTED, "GWSPaction(): Could not create message");
	xPL_setSchema(msg,"sensor","basic");
	xPL_setMessageNamedValue(msg, "device", "0"); 
	xPL_setMessageNamedValue(msg, "type", "windspeed");
	snprintf(ws, 10, "%3.1f", val);
	xPL_setMessageNamedValue(msg, "current", ws);
	xPL_setMessageNamedValue(msg, "units", (sp->units == KMH)? "kmh":"mph");
	xPL_sendMessage(msg);
	xPL_releaseMessage(msg);

}


/*
 * Act on response from GWDR command
 */

void GWDRAction(unsigned char pcount, responsePtr_t resp, workQEntryPtr_t wq)
{
	int msgType = xPL_MESSAGE_STATUS;
	char *wd;
	unsigned char dircode;
	xPL_MessagePtr msg = NULL;
	serviceEntryPtr_t sp = NULL;

	
	if((!resp) ||(!wq) || (!wq->sp))
		return;
		
	sp = wq->sp;
	
	if(pcount != 3){
		debug(DEBUG_UNEXPECTED, "GWDRAction(): Received an incorrect number of parameters, got %u, need 3", pcount);
		return;
	}
	dircode = resp->params[0];
	
	
	debug(DEBUG_ACTION, "GWDRAction(): Wind direction code = %u", dircode);
	
	
	/* Do conversion per units field */

	if(sp->units == _WDIRMAP){

		if(dircode < 16){
			wd = dirmap[dircode];
		}
	    else if(dircode == 0xfe)
			wd = "short";
		else if (dircode == 0xff)
			wd = "open";
		else
			wd = "error";
	}	
	else{
		wd = "badunits";
		debug(DEBUG_UNEXPECTED, "GWDRction(): Invalid unit for conversion");
	}
	
	debug(DEBUG_EXPECTED, "wd = %s", wd);
	
	if(wq->is_poll){ /* Was this the result of a poll */
		if(dircode == sp->poll_last) /* Was there a change ? */
			return;
		debug(DEBUG_EXPECTED, "Sending trigger");
		sp->poll_last = dircode;
		msgType = xPL_MESSAGE_TRIGGER;
	}
	

	
	if(!(msg = xPL_createBroadcastMessage(sp->xplService, msgType)))
		debug(DEBUG_UNEXPECTED, "GWDRaction(): Could not create message");
	xPL_setSchema(msg,"sensor","basic");
	xPL_setMessageNamedValue(msg, "device", "0"); 
	xPL_setMessageNamedValue(msg, "type", "winddir");
	xPL_setMessageNamedValue(msg, "current", wd);
	xPL_sendMessage(msg);
	xPL_releaseMessage(msg);

}

/*
 * Act on response from GRGC command
 */

void GRGCAction(unsigned char pcount, responsePtr_t resp, workQEntryPtr_t wq)
{
	float val;
	int msgType = xPL_MESSAGE_STATUS;
	char ws[12];
	uint_least32_t counts;
	uint_least16_t mantissa;
	int_least8_t exponent;
	xPL_MessagePtr msg = NULL;
	serviceEntryPtr_t sp = NULL;
	
	if((!resp) ||(!wq) || (!wq->sp))
		return;
		
	sp = wq->sp;
	
	if(pcount != 9){
		debug(DEBUG_UNEXPECTED, "GRGCAction(): Received an incorrect number of parameters, got %u, need 9", pcount);
		return;
	}
	exponent = (int_least8_t) resp->params[1];
	mantissa =  ((((uint_least16_t) resp->params[3]) << 8) + resp->params[2]);
	counts = ((uint32_t) resp->params[5]) + 
			  (((uint32_t) resp->params[6]) * 256 ) +
			  (((uint32_t) resp->params[7]) * 65536) +
			  (((uint32_t) resp->params[8]) * 16777216);

	printf("Raw counts  = %u", counts);
	
	/* Calculate result */
	val =  ((float)mantissa)*powf(10.0,exponent) * (float) counts;
	debug(DEBUG_ACTION, "GRGCAction(): Raingauge = %6.3f mm", val);
	
	
	
	/* Do conversion per units field */

	if (sp->units == MM);
	else if(sp->units == IN)
		val /= 25.4;
	else{
		debug(DEBUG_UNEXPECTED, "GRGCAction(): Invalid unit for conversion");
	}


	
	if(wq->is_poll){ /* Was this the result of a poll */
		if(val == sp->poll_f_last) /* Was there a change ? */
			return;
		debug(DEBUG_EXPECTED, "Sending trigger");
		sp->poll_f_last = val;
		msgType = xPL_MESSAGE_TRIGGER;
	}
	

	
	if(!(msg = xPL_createBroadcastMessage(sp->xplService, msgType)))
		debug(DEBUG_UNEXPECTED, "GRGCaction(): Could not create message");
	xPL_setSchema(msg,"sensor","basic");
	xPL_setMessageNamedValue(msg, "device", "0"); 
	xPL_setMessageNamedValue(msg, "type", "raingauge");
	snprintf(ws, 10, "%6.3f", val);
	xPL_setMessageNamedValue(msg, "current", ws);
	xPL_setMessageNamedValue(msg, "units", (sp->units == KMH)? "mm.":"in.");
	xPL_sendMessage(msg);
	xPL_releaseMessage(msg);

}



/*
 * Convert 2 characters into a uint_least8_t
 */
uint_least8_t hex2(String s)
{
	uint_least8_t  res = 0;
	int i;
	for(i = 0; i < 2; i++){
		res <<= 4;
		if((s[i] >= '0') &&  (s[i] <= '9'))
			res |= (uint_least8_t) (s[i] - '0');
		else if((s[i] >= 'A') && (s[i] <= 'F'))
			res |= (uint_least8_t) (s[i] - ('A' - 10));
		else
			break;
	}
	return res;	
}


/*
 * Decode the response, and figure out what to do with it
 */
 
static void decodeResponse(String r)
{
	int i, pcount;
	response_t response;
	
	debug(DEBUG_ACTION, "Line received: %s", r);
	if(!strncmp(r, "RS", 2)){
		response.address = hex2(r + 2);
		response.command = hex2(r + 4);
		pcount = (strlen(r) - 6) >> 1;
		for(i = 0; i < pcount; i++){
			response.params[i] = hex2(r + 6 + (i << 1));
		}
		debug_hexdump(DEBUG_ACTION, &response, pcount + 2, "Binary response dump: ");
		if((pendingResponse) && (pendingResponse->sp) && 
		(pendingResponse->sp->address == (unsigned) response.address)){
			switch((hanCommands_t) response.command){
				case GTMP: /* Temperature */
					GTMPAction(pcount, &response, pendingResponse);
					break;
					
				case GACD: /* AC voltage and frequency */
					GACDAction(pcount, &response, pendingResponse);
					break;
					
				case GOUT: /* Outputs */
					GOUTAction(pcount, &response, pendingResponse);
					break;
					
				case GVLT: /* Get voltage */
					GVLTAction(pcount, &response, pendingResponse);
					break;
					
				case GCUR: /* Get current */
					GCURAction(pcount, &response, pendingResponse);
					break;
					
			    case GHUM: /* Get Humidity */
					GHUMAction(pcount, &response, pendingResponse);
					break;
					
				case GWSP: /* Get wind speed */
					GWSPAction(pcount, &response, pendingResponse);
					break;
					
				case GWDR: /* Get wind direction */
					GWDRAction(pcount, &response, pendingResponse);
					break;
					
				case GRGC: /* Get rain gauge */
					GRGCAction(pcount, &response, pendingResponse);
					break;
					
				default:
					debug(DEBUG_UNEXPECTED, "Unknown response received");
					break;
			}
		}
		if(pendingResponse){ /* Free the work queue entry if it exists */
			freeWorkQueueEntry(pendingResponse);
			pendingResponse = NULL;
		}
	}
}
	

/*
 * Handler for han socket events
 */

static void hanHandler(int fd, int revents, int userValue)
{
	static unsigned pos = 0;
	static char response[WS_SIZE];
	int res;
	

	debug(DEBUG_ACTION,"revents = %08X", revents);
	
	
	res = socketReadLineNonBlocking(fd, &pos, response, WS_SIZE);
	if(res == -1)
		debug(DEBUG_UNEXPECTED, "Socket read returned error");
	else if (res == 1){
		if(!response[0]){
			/* EOF. We must close the socket and re-open it later */
			xPL_removeIODevice(hanSock);
			close(hanSock);
			hanSock = -1;
			cmdFail = TRUE;
			return;
		}
		decodeResponse(response);
	}
	
	
		
}

/* 
 * Queue GOUT Sensor Request
 */
 
static void qHanGOUT(unsigned subcommand, serviceEntryPtr_t sp, Bool isPoll)
{
	String cmd;
	
	/* Allocate buffer for command */
	if(!(cmd = mallocz(WS_SIZE)))
		MALLOC_ERROR;
	/* Format command */
	snprintf(cmd, WS_SIZE, "CA%02X%02X%02X%02X00",sp->address, (unsigned ) sp->cmd, sp->channel, subcommand );
	queueCommand(cmd, sp, isPoll);
}


/*
 * do HAN GOUT command
 */

static void doHanGOUT(xPL_MessagePtr theMessage, serviceEntryPtr_t sp)
{
	unsigned dev;
	unsigned outputState;
	
	

	
	const String device = xPL_getMessageNamedValue(theMessage, "device");

	
	if(!device){
		debug(DEBUG_UNEXPECTED,"doHanGout(): device missing, device required");
		return;
	}
	
	if(!str2uns(device, &dev, 0, MAX_HAN_DEVICE)){
		debug(DEBUG_UNEXPECTED,"doHanGout(): bad device number: %s", device);
		return;
	}		
		
	
	/* GOUT supports both control and sensor schemas */
	
	if(sp->is_sensor){ /* Sensor Request ? */
		const String request = xPL_getMessageNamedValue(theMessage, "request");
		if(!request){
			debug(DEBUG_UNEXPECTED,"doHanGout(): request missing, request required");
			return;
		}
			
		if(strcmp(request, "current")){
			debug(DEBUG_UNEXPECTED,"doHanGout(): only the current request is supported");
			return;
		}
		/* Queue command */
		qHanGOUT(2, sp, FALSE);
	}
	else{ /* Else assume control request */
		const String type = xPL_getMessageNamedValue(theMessage, "type");
		const String current = xPL_getMessageNamedValue(theMessage, "current");
	
		if(!type){
			debug(DEBUG_UNEXPECTED,"doHanGout(): type missing, type required");
			return;
		}
		
		if(!current){
			debug(DEBUG_UNEXPECTED,"doHanGout(): current missing, current required");
			return;
		}			
		
		if(strcmp(type, "output")){
			debug(DEBUG_UNEXPECTED,"doHanGout(): sensor type must be 'output'");
			return;
		}
		if(!strcmp(current, "high"))
			outputState = 1;
		else if(!strcmp(current, "low"))
			outputState = 0;
		else{
			debug(DEBUG_UNEXPECTED,"current must be one of: high, low");
			return;
		}
		qHanGOUT(outputState, sp, FALSE);
		
	}
	
}

/*
 * Queue han GACD command
 */
 
static void qHanGACD(serviceEntryPtr_t sp, Bool isPoll)
{
	String cmd;
	
	/* Allocate buffer for command */
	if(!(cmd = mallocz(WS_SIZE)))
		MALLOC_ERROR;
			
	/* Format command */
	snprintf(cmd, WS_SIZE, "CA%02X%02X00000000",sp->address, (unsigned ) sp->cmd);

	queueCommand(cmd, sp, isPoll);
	
}



/*
 * do HAN GACD command 
 */
 
static void doHanGACD(xPL_MessagePtr theMessage, serviceEntryPtr_t sp)
{

	const String request =  xPL_getMessageNamedValue(theMessage, "request");
	
	if(!request){
		debug(DEBUG_UNEXPECTED, "doHanGACD(): no request specified");
		return;
	}
	if(strcmp(request, "current")){ /* Only the current command is supported  */
		debug(DEBUG_UNEXPECTED, "doHanGACD(): only the current request is supported");
		return;
	}
	debug(DEBUG_ACTION, "doHanGACD()");	
	
	
	qHanGACD(sp, FALSE);
	
}

/*
 * Queue a HAN GTMP command
 */

static void qHanGTMP(serviceEntryPtr_t sp, Bool isPoll)
{
	String cmd;
	
	if(!sp)
		return;
		
	/* Allocate buffer for command */
	if(!(cmd = mallocz(WS_SIZE)))
		MALLOC_ERROR;
	
			
	/* Format command */
	snprintf(cmd, WS_SIZE, "CA%02X%02X%02X00000000",sp->address, (unsigned ) sp->cmd, sp->channel);

	queueCommand(cmd, sp, isPoll);
}


/*
 * Do HAN temperature command 
 */
 


static void doHanGTMP(xPL_MessagePtr theMessage, serviceEntryPtr_t sp)
{
	const String request =  xPL_getMessageNamedValue(theMessage, "request");
	const String device =  xPL_getMessageNamedValue(theMessage, "device");

	
	if(!request){
		debug(DEBUG_UNEXPECTED, "doHanGTMP(): no request specified");
		return;
	}
		
	if(!device){
		debug(DEBUG_UNEXPECTED, "doHanGTMP(): no device specified, device is required");

	}
	else{
		unsigned dev;
		if(!str2uns(device, &dev, 0, 0)){
			debug(DEBUG_UNEXPECTED, "doHanGTMP(): device=0 is required");
			return;
		}
	}
	
	if(strcmp(request, "current")){ 
		debug(DEBUG_UNEXPECTED, "doHanGTMP(): only the 'current' request is supported");
		return;
	}

		
	debug(DEBUG_ACTION, "doHanGTMP()");	

	qHanGTMP(sp, FALSE);
}

/*
 * Queue han GVLT command
 */
 
static void qHanGVLT(serviceEntryPtr_t sp, Bool isPoll)
{
	String cmd;
	
	/* Allocate buffer for command */
	if(!(cmd = mallocz(WS_SIZE)))
		MALLOC_ERROR;
			
	/* Format command */
	snprintf(cmd, WS_SIZE, "CA%02X%02X0000000000000000",sp->address, (unsigned ) sp->cmd);

	queueCommand(cmd, sp, isPoll);
	
}



/*
 * do HAN GVLT command 
 */
 
static void doHanGVLT(xPL_MessagePtr theMessage, serviceEntryPtr_t sp)
{

	const String request =  xPL_getMessageNamedValue(theMessage, "request");
	const String device =  xPL_getMessageNamedValue(theMessage, "device");
	
	if(!request){
		debug(DEBUG_UNEXPECTED, "doHanGVLT(): no request specified");
		return;
	}
	if(!device){
		debug(DEBUG_UNEXPECTED, "doHanGVLT(): no device specified, device is required");

	}
	else{
		unsigned dev;
		if(!str2uns(device, &dev, 0, 0)){
			debug(DEBUG_UNEXPECTED, "doHanGVLT(): device=0 is required");
			return;
		}
	}
	
	if(strcmp(request, "current")){ /* Only the current command is supported  */
		debug(DEBUG_UNEXPECTED, "doHanGVLT(): only the current request is supported");
		return;
	}
	debug(DEBUG_ACTION, "doHanGVLT()");	
	
	
	qHanGVLT(sp, FALSE);
	
}

/*
 * Queue han GCUR command
 */
 
static void qHanGCUR(serviceEntryPtr_t sp, Bool isPoll)
{
	String cmd;
	
	/* Allocate buffer for command */
	if(!(cmd = mallocz(WS_SIZE)))
		MALLOC_ERROR;
			
	/* Format command */
	snprintf(cmd, WS_SIZE, "CA%02X%02X0000000000000000",sp->address, (unsigned ) sp->cmd);

	queueCommand(cmd, sp, isPoll);
	
}



/*
 * do HAN GCUR command 
 */
 
static void doHanGCUR(xPL_MessagePtr theMessage, serviceEntryPtr_t sp)
{

	const String request =  xPL_getMessageNamedValue(theMessage, "request");
	const String device =  xPL_getMessageNamedValue(theMessage, "device");
	
	if(!request){
		debug(DEBUG_UNEXPECTED, "doHanGCUR(): no request specified");
		return;
	}
	if(!device){
		debug(DEBUG_UNEXPECTED, "doHanGCUR(): no device specified, device is required");

	}
	else{
		unsigned dev;
		if(!str2uns(device, &dev, 0, 0)){
			debug(DEBUG_UNEXPECTED, "doHanGCUR(): device=0 is required");
			return;
		}
	}
	
	if(strcmp(request, "current")){ /* Only the current command is supported  */
		debug(DEBUG_UNEXPECTED, "doHanGCUR(): only the current request is supported");
		return;
	}
	debug(DEBUG_ACTION, "doHanGCUR()");	
	
	
	qHanGCUR(sp, FALSE);
	
}

/*
 * Queue a HAN GHUM command
 */

static void qHanGHUM(serviceEntryPtr_t sp, Bool isPoll)
{
	String cmd;
	
	if(!sp)
		return;
		
	/* Allocate buffer for command */
	if(!(cmd = mallocz(WS_SIZE)))
		MALLOC_ERROR;
	
			
	/* Format command */
	snprintf(cmd, WS_SIZE, "CA%02X%02X%02X0000000000",sp->address, (unsigned ) sp->cmd, sp->channel);

	queueCommand(cmd, sp, isPoll);
}


/*
 * Do HAN humidity command 
 */
 


static void doHanGHUM(xPL_MessagePtr theMessage, serviceEntryPtr_t sp)
{
	const String request =  xPL_getMessageNamedValue(theMessage, "request");
	const String device =  xPL_getMessageNamedValue(theMessage, "device");

	
	if(!request){
		debug(DEBUG_UNEXPECTED, "doHanGHUM(): no request specified");
		return;
	}
		
	if(!device){
		debug(DEBUG_UNEXPECTED, "doHanGHUM(): no device specified, device is required");

	}
	else{
		unsigned dev;
		if(!str2uns(device, &dev, 0, 0)){
			debug(DEBUG_UNEXPECTED, "doHanGHUM(): device=0 is required");
			return;
		}
	}
	
	if(strcmp(request, "current")){ 
		debug(DEBUG_UNEXPECTED, "doHanGHUM(): only the 'current' request is supported");
		return;
	}

		
	debug(DEBUG_ACTION, "doHanGHUM()");	

	qHanGHUM(sp, FALSE);
}

/*
 * Queue a HAN GWSP command
 */

static void qHanGWSP(serviceEntryPtr_t sp, Bool isPoll)
{
	String cmd;
	
	if(!sp)
		return;
		
	/* Allocate buffer for command */
	if(!(cmd = mallocz(WS_SIZE)))
		MALLOC_ERROR;
	
			
	/* Format command */
	snprintf(cmd, WS_SIZE, "CA%02X%02X%02x0000000000",sp->address, (unsigned ) sp->cmd, sp->channel);

	queueCommand(cmd, sp, isPoll);
}


/*
 * Do HAN wind speed command
 */
 


static void doHanGWSP(xPL_MessagePtr theMessage, serviceEntryPtr_t sp)
{
	const String request =  xPL_getMessageNamedValue(theMessage, "request");
	const String device =  xPL_getMessageNamedValue(theMessage, "device");

	
	if(!request){
		debug(DEBUG_UNEXPECTED, "doHanGWSP(): no request specified");
		return;
	}
		
	if(!device){
		debug(DEBUG_UNEXPECTED, "doHanGWSP(): no device specified, device is required");

	}
	else{
		unsigned dev;
		if(!str2uns(device, &dev, 0, 0)){
			debug(DEBUG_UNEXPECTED, "doHanGWSP(): device=0 is required");
			return;
		}
	}
	
	if(strcmp(request, "current")){ 
		debug(DEBUG_UNEXPECTED, "doHanGWSP(): only the 'current' request is supported");
		return;
	}

		
	debug(DEBUG_ACTION, "doHanGWSP()");	

	qHanGWSP(sp, FALSE);
}


/*
 * Queue a HAN Wind direction command
 */

static void qHanGWDR(serviceEntryPtr_t sp, Bool isPoll)
{
	String cmd;
	
	if(!sp)
		return;
		
	/* Allocate buffer for command */
	if(!(cmd = mallocz(WS_SIZE)))
		MALLOC_ERROR;
	
			
	/* Format command */
	snprintf(cmd, WS_SIZE, "CA%02X%02X000000",sp->address, (unsigned ) sp->cmd);

	queueCommand(cmd, sp, isPoll);
}


/*
 * Do HAN wind direction command 
 */
 


static void doHanGWDR(xPL_MessagePtr theMessage, serviceEntryPtr_t sp)
{
	const String request =  xPL_getMessageNamedValue(theMessage, "request");
	const String device =  xPL_getMessageNamedValue(theMessage, "device");

	
	if(!request){
		debug(DEBUG_UNEXPECTED, "doHanGWDR(): no request specified");
		return;
	}
		
	if(!device){
		debug(DEBUG_UNEXPECTED, "doHanGWDR(): no device specified, device is required");

	}
	else{
		unsigned dev;
		if(!str2uns(device, &dev, 0, 0)){
			debug(DEBUG_UNEXPECTED, "doHanGWDR(): device=0 is required");
			return;
		}
	}
	
	if(strcmp(request, "current")){ 
		debug(DEBUG_UNEXPECTED, "doHanGWDR(): only the 'current' request is supported");
		return;
	}

		
	debug(DEBUG_ACTION, "doHanGWDR()");	

	qHanGWDR(sp, FALSE);
}


/*
 * Queue a HAN rain gauge command
 */

static void qHanGRGC(serviceEntryPtr_t sp, Bool isPoll)
{
	String cmd;
	
	if(!sp)
		return;
		
	/* Allocate buffer for command */
	if(!(cmd = mallocz(WS_SIZE)))
		MALLOC_ERROR;
	
			
	/* Format command */
	snprintf(cmd, WS_SIZE, "CA%02X%02X00000000%02X00000000",sp->address, (unsigned ) sp->cmd, sp->channel);

	queueCommand(cmd, sp, isPoll);
}


/*
 * Do rain gauge command
 */
 


static void doHanGRGC(xPL_MessagePtr theMessage, serviceEntryPtr_t sp)
{
	const String request =  xPL_getMessageNamedValue(theMessage, "request");
	const String device =  xPL_getMessageNamedValue(theMessage, "device");

	
	if(!request){
		debug(DEBUG_UNEXPECTED, "doHanGRGC(): no request specified");
		return;
	}
		
	if(!device){
		debug(DEBUG_UNEXPECTED, "doHanGRGC(): no device specified, device is required");

	}
	else{
		unsigned dev;
		if(!str2uns(device, &dev, 0, 0)){
			debug(DEBUG_UNEXPECTED, "doHanGRGC(): device=0 is required");
			return;
		}
	}
	
	if(strcmp(request, "current")){ 
		debug(DEBUG_UNEXPECTED, "doHanGRGC(): only the 'current' request is supported");
		return;
	}

		
	debug(DEBUG_ACTION, "doHanGRGC()");	

	qHanGRGC(sp, FALSE);
}


/*
 * Poll dispatcher
 */ 

static void dispatchPollCommand(serviceEntryPtr_t sp)
{
	if(!sp)
		return;
	switch(sp->cmd){
		case GTMP:
			qHanGTMP(sp, TRUE);
			break;
			
		case GACD:
			qHanGACD(sp, TRUE);
			break;
			
		case GOUT:
			qHanGOUT(2, sp, TRUE);
			break;
			
		case GVLT:
			qHanGVLT(sp, TRUE);
			break;
		
		case GCUR:
			qHanGCUR(sp, TRUE);
			break;
			
		case GHUM:
			qHanGHUM(sp, TRUE);
			break;
			
		case GWSP:
			qHanGWSP(sp, TRUE);
			break;
			
		case GWDR:
			qHanGWDR(sp, TRUE);
			break;
		
	    case GRGC:
			qHanGRGC(sp, TRUE);
			break;
		
		
		default:
			debug(DEBUG_UNEXPECTED, "Got unrecognized poll request %u", (unsigned) sp->cmd);
			break;
	}	
}



/* 
 * HAN Command dispatcher
 */

static void dispatchHanCommand(xPL_MessagePtr theMessage, serviceEntryPtr_t sp)
{
	if((!theMessage) || (!sp))
		return;
		
	debug(DEBUG_ACTION, "dispatchHanCommand()");
	
	switch(sp->cmd){
		case GTMP:
			doHanGTMP(theMessage, sp);
			break;
		
		case GACD:
			doHanGACD(theMessage, sp);
			break;
			
		case GOUT:
			doHanGOUT(theMessage, sp);
			break;
			
		case GVLT:
			doHanGVLT(theMessage, sp);
			break;
			
		case GCUR:
			doHanGCUR(theMessage, sp);
			break;
			
	    case GHUM:
			doHanGHUM(theMessage, sp);
			break;
			
		case GWSP:
			doHanGWSP(theMessage, sp);
			break;
			
		case GWDR:
			doHanGWDR(theMessage, sp);
			break;
			
		case GRGC:
			doHanGRGC(theMessage, sp);
			break;
					
		default:
			debug(DEBUG_UNEXPECTED,"Invalid han command received: %02X", (unsigned) sp->cmd);
			break; 
	}
}


/*
* Our Listener 
*/

static void xPLListener(xPL_MessagePtr theMessage, xPL_ObjectPtr userValue)
{
	uint32_t iid_hash;
	serviceEntryPtr_t sp;

	
	if(!xPL_isBroadcastMessage(theMessage)){ /* If not a broadcast message */
		if(xPL_MESSAGE_COMMAND == xPL_getMessageType(theMessage)){ /* If the message is a command */
			const String type = xPL_getSchemaType(theMessage);
			const String class = xPL_getSchemaClass(theMessage);
			const String instanceID = xPL_getTargetInstanceID(theMessage);
			debug(DEBUG_ACTION, "Received message from: %s-%s.%s,\n instance id: %s, class: %s, type: %s",
			xPL_getSourceVendor(theMessage), xPL_getSourceDeviceID(theMessage),
			xPL_getSourceInstanceID(theMessage), instanceID, class, type);
			
			/* Hash the incoming instance ID */
			iid_hash = confreadHash(instanceID);
			
			/* Traverse the service list looking for an instance ID which matches */
			for(sp = serviceEntryHead; sp; sp = sp->next){
				if((iid_hash == sp->iid_hash) && (!strcmp(instanceID, sp->instance_id)))
					break;
	
			}
			if((sp) && (!strcmp(class, sp->class)) && (!strcmp(type, sp->type)))
				dispatchHanCommand(theMessage, sp);	
			
		}
	}
}



/*
* Our tick handler. 
* This is used check for commands to send to the HAN server.
* If one is present, it is sent, and then dequeued and freed.
*/

static void tickHandler(int userVal, xPL_ObjectPtr obj)
{
	serviceEntryPtr_t se;
	
	/* debug(DEBUG_ACTION,"TICK"); */
	
	/* Traverse the service list looking for expired poll counters */
	
	for(se = serviceEntryHead; se; se = se->next){
		if(se->polling_interval){
			if(!se->poll_counter){
				se->poll_counter = se->polling_interval;
				dispatchPollCommand(se);
			}
			else
				se->poll_counter--;
		}
	}
			

	
	if(workQTail){
		if(hanSock == -1){ /* Socket not connected. This could have been due to an EOF detected previously */
			if((hanSock = socketConnectIP(host, service, PF_UNSPEC, SOCK_STREAM)) < 0){
				debug(DEBUG_UNEXPECTED, "Could not open socket to han server (post fork)");
				freeWorkQueueEntry(dequeueWorkQueueEntry()); /* Can't process command */
				cmdFail = TRUE;
				/* FIXME: Need to find some way to notify the originator the command could not be completed */
				return;
			}
			cmdFail = FALSE;
			/* Add han socket to the xPL polling list */
			if(xPL_addIODevice(hanHandler, 1234, hanSock, TRUE, FALSE, FALSE) == FALSE)
				fatal("Could not register han socket fd with xPL");
		}
		debug(DEBUG_ACTION, "Sending command: %s", workQTail->cmd);
		if(socketPrintf(hanSock, "%s", workQTail->cmd) < 0){ /* Send the command */
			debug(DEBUG_UNEXPECTED, "Command TX failed");
			xPL_removeIODevice(hanSock);
			close(hanSock);
			hanSock = -1;
			cmdFail = TRUE;
		}
		pendingResponse = workQTail;
		dequeueWorkQueueEntry(); /* Remove command from queue */
	}
}


/*
* Show help
*/

void showHelp(void)
{
	printf("'%s' is a daemon that XXXXXXXXXX\n", progName);
	printf("via XXXXXXXXXXX\n");
	printf("\n");
	printf("Usage: %s [OPTION]...\n", progName);
	printf("\n");
	printf("  -c, --config-file PATH  Set the path to the config file\n");
	printf("  -d, --debug LEVEL       Set the debug level, 0 is off, the\n");
	printf("                          compiled-in default is %d and the max\n", debugLvl);
	printf("                          level allowed is %d\n", DEBUG_MAX);
	printf("  -f, --pid-file PATH     Set new pid file path, default is: %s\n", pidFile);
	printf("  -h, --help              Shows this\n");
	printf("  -i, --interface NAME    Set the broadcast interface (e.g. eth0)\n");
	printf("  -l, --log  PATH         Path name to debug log file when daemonized\n");
	printf("  -n, --no-background     Do not fork into the background (useful for debugging)\n");
	printf("  -s, --instance ID       Set instance id. Default is %s", instanceID);
	printf("  -v, --version           Display program version\n");
	printf("\n");
 	printf("Report bugs to <%s>\n\n", EMAIL);
	return;

}


/*
* main
*/


int main(int argc, char *argv[])
{
	int longindex;
	int optchar;
	int i,j;
	int serviceCount;
	String p;
	SectionEntryPtr_t se;
	serviceEntryPtr_t sp;
	String slist[MAX_SERVICES];

		

	/* Set the program name */
	progName=argv[0];

	/* Parse the arguments. */
	while((optchar=getopt_long(argc, argv, SHORT_OPTIONS, longOptions, &longindex)) != EOF) {
		
		/* Handle each argument. */
		switch(optchar) {
			
				/* Was it a long option? */
			case 0:
				
				/* Hrmm, something we don't know about? */
				fatal("Unhandled long getopt option '%s'", longOptions[longindex].name);
			
				/* If it was an error, exit right here. */
			case '?':
				exit(1);
		
				/* Was it a config file switch? */
			case 'c':
				confreadStringCopy(configFile, optarg, WS_SIZE - 1);
				debug(DEBUG_ACTION,"New config file path is: %s", configFile);
				break;
				
				/* Was it a debug level set? */
			case 'd':

				/* Save the value. */
				debugLvl=atoi(optarg);
				if(debugLvl < 0 || debugLvl > DEBUG_MAX) {
					fatal("Invalid debug level");
				}

				break;

			/* Was it a pid file switch? */
			case 'f':
				confreadStringCopy(pidFile, optarg, WS_SIZE - 1);
				clOverride.pid_file = 1;
				debug(DEBUG_ACTION,"New pid file path is: %s", pidFile);
				break;
			
				/* Was it a help request? */
			case 'h':
				showHelp();
				exit(0);

				/* Specify interface to broadcast on */
			case 'i': 
				confreadStringCopy(interface, optarg, WS_SIZE -1);
				clOverride.interface = 1;
				break;

			case 'l':
				/* Override log path*/
				confreadStringCopy(logPath, optarg, WS_SIZE - 1);
				clOverride.log_path = 1;
				debug(DEBUG_ACTION,"New log path is: %s",
				logPath);

				break;

				/* Was it a no-backgrounding request? */
			case 'n':
				/* Mark that we shouldn't background. */
				noBackground = TRUE;
				break;
	
						
				/* Was it an instance ID ? */
			case 's':
				confreadStringCopy(instanceID, optarg, WS_SIZE);
				clOverride.instance_id = 1;
				debug(DEBUG_ACTION,"New instance ID is: %s", instanceID);
				break;


				/* Was it a version request? */
			case 'v':
				printf("Version: %s\n", VERSION);
				exit(0);
	

			
				/* It was something weird.. */
			default:
				fatal("Unhandled getopt return value %d", optchar);
		}
	}

	
	/* If there were any extra arguments, we should complain. */

	if(optind < argc) {
		fatal("Extra argument on commandline, '%s'", argv[optind]);
	}

	/* Attempt to read a config file */
	
	if(!(configEntry =confreadScan(configFile, NULL)))
		exit(1);
	
	/* Attempt to get general stanza */	
	if(!(se = confreadFindSection(configEntry, "general")))
		fatal("Error in config file: general stanza does not exist");

	/* Host */
	if((p = confreadValueBySectEntKey(se, "host")))
		confreadStringCopy(host, p, WS_SIZE);
		
	/* Port/Service */
	if((p = confreadValueBySectEntKey(se, "port")))
		confreadStringCopy(service, p, WS_SIZE);
	
	
			
	/* Instance ID */
	if((!clOverride.instance_id) && (p = confreadValueBySectEntKey(se, "instance-id")))
		confreadStringCopy(instanceID, p, sizeof(instanceID));
		
	/* Interface */
	if((!clOverride.interface) && (p = confreadValueBySectEntKey(se, "interface")))
		confreadStringCopy(interface, p, sizeof(interface));
			
	/* pid file */
	if((!clOverride.pid_file) && (p = confreadValueBySectEntKey(se, "pid-file")))
		confreadStringCopy(pidFile, p, sizeof(pidFile));	
						
	/* log path */
	if((!clOverride.log_path) && (p = confreadValueBySectEntKey(se, "log-path")))
		confreadStringCopy(logPath, p, sizeof(logPath));
			
	/* Build the instance list */
	if(!(p = confreadValueBySectEntKey(se, "services")))
		fatal("At least one service must be defined in the general section");
	serviceCount = dupOrSplitString(p, slist, ',', MAX_SERVICES);
	
	for(i = 0; i < serviceCount; i++){
	
		if(!(se = confreadFindSection(configEntry, slist[i])))
			fatal("Stanza for service %s does not exist", slist[i]);
		if(!(p = confreadValueBySectEntKey(se, "instance")))
			fatal("Instance for service %s does not exist", slist[i]);
		
		/* Check for duplicate service name  or duplicate instance id */
		for(j = 0, sp = serviceEntryHead; j < i ; j++){
				if(!strcmp(slist[i], slist[j]))
					fatal("Service name %s is already defined", slist[j]);
				if(sp){	
					if(!strcmp(p, sp->instance_id))
						fatal("Instance id %s is already defined", p);
					sp= sp->next;
				}
		}
						
		/* Allocate a data structure */
		if(!(sp = mallocz(sizeof(serviceEntry_t))))
			MALLOC_ERROR;
			
		/* Save instance ID */	
		if(!(sp->instance_id = strdup(p))) 
			MALLOC_ERROR;
		/* Hash  instance ID */
		sp->iid_hash = confreadHash(sp->instance_id);
			
		/* Get Address */
		if(!(p = confreadValueBySectEntKey(se, "address")))
			fatal("Address missing in stanza: %s", slist[i]);
		if(!str2uns(p, &sp->address, 0, 254))
			fatal("In stanza %s, the address must be between 0 and 254", slist[i]);
		
		/* Get class */
		if(!(p = confreadValueBySectEntKey(se, "class")))
			fatal("class missing in stanza: %s", slist[i]);
		if(!(sp->class = strdup(p)))
			MALLOC_ERROR;
		if(!strcmp(sp->class, "sensor")) /* Set flag if sensor */
			sp->is_sensor = TRUE;
	
		
			
		/* Get type */
		if(!(p = confreadValueBySectEntKey(se, "type")))
			fatal("type missing in stanza: %s", slist[i]);
		if(!(sp->type = strdup(p)))
			MALLOC_ERROR;
			
		/* Map han command */
		if(!(p = confreadValueBySectEntKey(se, "han-command")))
			fatal("han-command missing in stanza: %s", slist[i]);		
		for(j = 0; hanCommandMap[j].code ; j++){
			if(!strcmp(p, hanCommandMap[j].keyword))
				break;
		}
		if(!(sp->cmd = hanCommandMap[j].code))
			fatal("Unrecognized han-command: %s in stanza: %s", p, slist[i]);
			
		/* Map units, if class is 'sensor' */
		if(sp->is_sensor){
			if(!(p = confreadValueBySectEntKey(se, "units")))
				fatal("units missing in stanza: %s", slist[i]);			
			for(j = 0; unitsMap[j].code ; j++){
				if(!strcmp(p, unitsMap[j].keyword))
					break;
			}
	
		}
		if(!(sp->units = unitsMap[j].code)) //??????????
				fatal("Unrecognized units: %s in stanza: %s", p, slist[i]);		
			
		/* Check poll-interval if present and class is sensor */
		
		if((p = confreadValueBySectEntKey(se, "polling-interval"))){
			if(!sp->is_sensor)
				fatal("In stanza %s, a polling-interval is specified for non-sensor service", slist[i]);
			if(!str2uns(p, &sp->polling_interval, 0,  MAX_POLL_INTERVAL))
				fatal("In stanza %s, polling-interval must be between 0 and %u", slist[i], MAX_POLL_INTERVAL);
		}
		
		
		/* Check channel if present */
		
		if((p = confreadValueBySectEntKey(se, "channel"))){
			if(!str2uns(p, &sp->channel, 0,  MAX_POLL_INTERVAL))
				fatal("In stanza %s, channel must be between 0 and %u", slist[i], MAX_CHANNEL);
		}
			
		/* Add the service ID */
		sp->service_id = i;	
		
		/* Insert entry into service list */
		if(!serviceEntryHead)
			serviceEntryHead = serviceEntryTail = sp;
		else{
			serviceEntryTail->next = sp;
			sp->prev = serviceEntryTail;
			serviceEntryTail = sp;
		}	
	}
	free(slist[0]); /* Free service list */
	
	/*
	 * Sanity check the command to units mapping if class is 'sensor'
	 */
	 
	 for(sp = serviceEntryHead; sp; sp = sp->next){
		 if(sp->is_sensor){
			for(i = 0; hanCommandMap[i].code; i++){
				if(hanCommandMap[i].code == sp->cmd){
					for(j = 0; hanCommandMap[i].valid_units[j]; j++){
						if(sp->units == hanCommandMap[i].valid_units[j]){
							break;
						}
					}
					if(!hanCommandMap[i].valid_units[j]){
						fatal("Instance %s fails sanity check of han command to units", sp->instance_id);
					}
				}
			}
		}		
	}
	
	/*
	 * Do a test connect to the han server
	 */
 
	if((hanSock = socketConnectIP(host, service, PF_UNSPEC, SOCK_STREAM)) < 0)
		fatal("Could not connect to han server");
	close(hanSock);
	hanSock = -1;


	/* Turn on library debugging for level 5 */
	if(debugLvl >= 5)
		xPL_setDebugging(TRUE);

  	/* Make sure we are not already running (.pid file check). */
	if(pid_read(pidFile) != -1) {
		fatal("%s is already running", progName);
	}

	/* Fork into the background. */

	if(!noBackground) {
		int retval;
		debug(DEBUG_STATUS, "Forking into background");

    	/* 
		* If debugging is enabled, and we are daemonized, redirect the debug output to a log file if
    	* the path to the logfile is defined
		*/

		if((debugLvl) && (logPath[0]))                          
			notify_logpath(logPath);
			
	
		/* Fork and exit the parent */

		if((retval = fork())){
      			if(retval > 0)
				exit(0);  /* Exit parent */
			else
				fatal_with_reason(errno, "parent fork");
    		}



		/*
		* The child creates a new session leader
		* This divorces us from the controlling TTY
		*/

		if(setsid() == -1)
			fatal_with_reason(errno, "creating session leader with setsid");


		/*
		* Fork and exit the session leader, this prohibits
		* reattachment of a controlling TTY.
		*/

		if((retval = fork())){
			if(retval > 0)
        			exit(0); /* exit session leader */
			else
				fatal_with_reason(errno, "session leader fork");
		}

		/* 
		* Change to the root of all file systems to
		* prevent mount/unmount problems.
		*/

		if(chdir("/"))
			fatal_with_reason(errno, "chdir to /");

		/* set the desired umask bits */

		umask(022);
		
		/* Close STDIN, STDOUT, and STDERR */

		close(0);
		close(1);
		close(2);
		} 

	/* Set the xPL interface */
	xPL_setBroadcastInterface(interface);

	/* Start xPL up */
	if (!xPL_initialize(xPL_getParsedConnectionType())) {
		fatal("Unable to start xPL lib");
	}

	/* Initialize xplrcs service */

	/* Create virtual services and set our application version */
	for(sp = serviceEntryHead; sp; sp = sp->next){
		debug(DEBUG_EXPECTED, "Creating xplhan service with class: %s type: %s with instance ID: %s", sp->class, sp->type, sp->instance_id);
		sp->xplService = xPL_createService("hwstar", "xplhan", sp->instance_id);
		xPL_setServiceVersion(sp->xplService, VERSION);
	}


  	/* Install signal traps for proper shutdown */
 	signal(SIGTERM, shutdownHandler);
 	signal(SIGINT, shutdownHandler);
 
	/* Add 1 second tick service */
	xPL_addTimeoutHandler(tickHandler, 1, NULL);

  	/* And a listener for all xPL messages */
  	xPL_addMessageListener(xPLListener, NULL);


 	/* Enable the service */
 	for(sp = serviceEntryHead; sp; sp = sp->next){
		xPL_setServiceEnabled(sp->xplService, TRUE);
	}

	if(pid_write(pidFile, getpid()) != 0) {
		debug(DEBUG_UNEXPECTED, "Could not write pid file '%s'.", pidFile);
	}




 	/** Main Loop **/

	for (;;) {
		/* Let XPL run forever */
		xPL_processMessages(-1);
  	}

	exit(1);
	return 1;
}

