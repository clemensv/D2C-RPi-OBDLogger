/* Copyright 2009 Gary Briggs

This file is part of obdgpslogger.

obdgpslogger is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

obdgpslogger is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with obdgpslogger.  If not, see <http://www.gnu.org/licenses/>.
*/


/** \file
 \brief main obdlogger entrypoint
 */

// Some references:
// mpg calculation: http://www.mp3car.com/vbulletin/engine-management-obd-ii-engine-diagnostics-etc/75138-calculating-mpg-vss-maf-obd2.html
// function list: http://www.kbmsystems.net/obd_tech.htm

#include "obdconfig.h"
#include "main.h"
#include "obdservicecommands.h"
#include "obdserial.h"
#include "gpscomm.h"
#include "supportedcommands.h"

#include "obdconfigfile.h"

#ifdef HAVE_GPSD
#include "gps.h"

/// IP address of gpsd
#define GPSD_ADDR "127.0.0.1"

/// Port of gpsd
#define GPSD_PORT "2947"
#endif //HAVE_GPSD

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <time.h>
#include <getopt.h>
#include <unistd.h>
#include <uuid/uuid.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <proton/message.h>
#include <proton/messenger.h>

#ifdef HAVE_SIGNAL_H
#include <signal.h>
#endif // HAVE_SIGNAL_H

/// Set when we catch a signal we want to exit on
static int receive_exitsignal = 0;

/// If we catch a signal to start the trip, set this
static int sig_starttrip = 0;

#ifdef OBDPLATFORM_POSIX
/// Daemonise. Returns 0 for success, or nonzero on failure.
static int obddaemonise();
#endif //OBDPLATFORM_POSIX

/// Set up signal handling
static void install_signalhandlers();

static void catch_quitsignal(int sig)
{
	receive_exitsignal = 1;
}

static void catch_tripstartsignal(int sig)
{
	sig_starttrip = 1;
}

#define check(messenger)                                                     \
	{                                                                          \
		if(pn_messenger_errno(messenger))                                        \
		{                                                                        \
			logmsgerr(__FILE__, __LINE__, pn_error_text(pn_messenger_error(messenger))); \
		}                                                                        \
	}                                                                          \
	 
void logmsgerr(const char *file, int line, const char *message)
{
	fprintf(stderr, "%s:%i: %s\n", file, line, message);
}


int main(int argc, char** argv)
{
	int maxMsgPayload = 1 * 1024;

	/// Serial port full path to open
	char *serialport = NULL;

	/// Database file to open
	char *queueAddress = NULL;

	/// List of columsn to log
	char *log_columns = NULL;

	/// Number of samples to take
	int samplecount = -1;

	/// Number of samples per second
	int samplespersecond = 10;

	/// Ask to show the capabilities of the OBD device then exit
	int showcapabilities = 0;

	/// Set if the user wishes to upgrade the baudrate
	long baudrate_upgrade = -1;

	/// Time between samples, measured in microseconds
	long frametime = 0;

	/// Spam all readings to stdout
	int spam_stdout = 0;

	/// Enable elm optimisations
	int enable_optimisations = 0;

	/// Enable serial logging
	int enable_seriallog = 0;

	/// Serial log filename
	char *seriallogname = NULL;

	uuid_t driveid;

#ifdef OBDPLATFORM_POSIX
	/// Daemonise
	int daemonise = 0;
#endif //OBDPLATFORM_POSIX

	/// Requested baudrate
	long requested_baud = -1;

	// Config File
	struct OBDGPSConfig *obd_config = obd_loadConfig(0);

	if(NULL != obd_config) {
		samplespersecond = obd_config->samplerate;
		enable_optimisations = obd_config->optimisations;
		requested_baud = obd_config->baudrate;
		baudrate_upgrade = obd_config->baudrate_upgrade;
	}

	uuid_generate(driveid);

	// Do not attempt to buffer stdout at all
	setvbuf(stdout, (char *)NULL, _IONBF, 0);

	int optc;
	int mustexit = 0;
	while ((optc = getopt_long (argc, argv, shortopts, longopts, NULL)) != -1) {
		switch (optc) {
		case 'h':
			printhelp(argv[0]);
			mustexit = 1;
			break;
		case 'v':
			printversion();
			mustexit = 1;
			break;
		case 's':
			if(NULL != serialport) {
				free(serialport);
			}
			serialport = strdup(optarg);
			break;
		case 'o':
			enable_optimisations = 1;
			break;
		case 't':
			spam_stdout = 1;
			break;
		case 'u': {
			int newout = open(optarg, O_CREAT|O_RDWR|O_APPEND,
			                  S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH);
			if(-1 == newout) {
				perror(optarg);
			} else {
				printf("Redirecting output to %s\n", optarg);
				close(STDOUT_FILENO);
				close(STDERR_FILENO);
				dup2(newout, STDOUT_FILENO);
				dup2(newout, STDERR_FILENO);
			}
		}
		break;
#ifdef OBDPLATFORM_POSIX
		case 'm':
			daemonise = 1;
			break;
#endif //OBDPLATFORM_POSIX
		case 'c':
			samplecount = atoi(optarg);
			break;
		case 'b':
			requested_baud = strtol(optarg, (char **)NULL, 10);
			break;
		case 'B':
			baudrate_upgrade = strtol(optarg, (char **)NULL, 10);
			break;
		case 'q':
			if(NULL != queueAddress) {
				free(queueAddress);
			}
			queueAddress = strdup(optarg);
			break;
		case 'i':
			if(NULL != log_columns) {
				free(log_columns);
			}
			log_columns = strdup(optarg);
			break;
		case 'a':
			samplespersecond = atoi(optarg);
			break;
		case 'l':
			enable_seriallog = 1;
			if(NULL != seriallogname) {
				free(seriallogname);
			}
			seriallogname = strdup(optarg);
			break;
		case 'p':
			showcapabilities = 1;
			break;
		default:
			mustexit = 1;
			break;
		}
	}

	if(mustexit) exit(0);

	if(0 >= samplespersecond) {
		frametime = 0;
	} else {
		frametime = 1000000 / samplespersecond;
	}

	if(NULL == serialport) {
		if(NULL != obd_config && NULL != obd_config->obd_device) {
			serialport = strdup(obd_config->obd_device);
		} else {
			serialport = strdup(OBD_DEFAULT_SERIALPORT);
		}
	}
	if(NULL == queueAddress) {
		if(NULL != obd_config && NULL != obd_config->log_file) {
			queueAddress = strdup(obd_config->log_file);
		} else {
			fprintf(stderr, "No queue address.\n");
			exit(1);
		}
	}
	if(NULL == log_columns) {
		if(NULL != obd_config && NULL != obd_config->log_columns) {
			log_columns = strdup(obd_config->log_columns);
		} else {
			log_columns = strdup(OBD_DEFAULT_COLUMNS);
		}
	}


	if(enable_seriallog && NULL != seriallogname) {
		startseriallog(seriallogname);
	}


	// Open the serial port.
	int obd_serial_port = openserial(serialport, requested_baud, baudrate_upgrade);

	if(-1 == obd_serial_port) {
		fprintf(stderr, "Couldn't open obd serial port. Attempting to continue.\n");
	} else {
		fprintf(stderr, "Successfully connected to serial port. Will log obd data\n");
	}

	// Just figure out our car's OBD port capabilities and print them
	if(showcapabilities) {
		printobdcapabilities(obd_serial_port);

		printf("\n");
		closeserial(obd_serial_port);
		exit(0);
	}


#ifdef HAVE_GPSD
	// Open the gps device
	struct gps_data_t *gpsdata;
	gpsdata = opengps(GPSD_ADDR, GPSD_PORT);

	if(NULL == gpsdata) {
		fprintf(stderr, "Couldn't open gps port on startup.\n");
	} else {
		fprintf(stderr, "Successfully connected to gpsd. Will log gps data\n");
	}

#endif //HAVE_GPSD

	if(-1 == obd_serial_port
#ifdef HAVE_GPSD
	   && NULL == gpsdata
#endif //HAVE_GPSD
	  ) {
		fprintf(stderr, "Couldn't find either gps or obd to log. Exiting.\n");
		exit(1);
	}


	// Wishlist of commands from config file
	struct obdservicecmd **wishlist_cmds = NULL;
	obd_configCmds(log_columns, &wishlist_cmds);

	void *obdcaps = getobdcapabilities(obd_serial_port,wishlist_cmds);

	obd_freeConfigCmds(wishlist_cmds);
	wishlist_cmds=NULL;

	int q;
	int totalSupportedObdCommands = 0;
	for(q=0; q<sizeof(obdcmds_mode1)/sizeof(obdcmds_mode1[0]); q++) {
		if(NULL != obdcmds_mode1[q].db_column  && isobdcapabilitysupported(obdcaps,q)) {
			totalSupportedObdCommands++;
		}
	}
	int supportedObdCommandsMap[totalSupportedObdCommands]; // Commands to send [index into obdcmds_mode1]

	int i,j;
	for(i=0,j=0; i<sizeof(obdcmds_mode1)/sizeof(obdcmds_mode1[0]); i++) {
		if(NULL != obdcmds_mode1[i].db_column) {
			if(isobdcapabilitysupported(obdcaps,i)) {
				supportedObdCommandsMap[j] = i;
				j++;
			}
		}
	}

	freeobdcapabilities(obdcaps);


#ifdef OBDPLATFORM_POSIX
	if(daemonise) {
		if(0 != obddaemonise()) {
			fprintf(stderr,"Couldn't daemonise, exiting\n");
			closeserial(obd_serial_port);
			exit(1);
		}
	}
#endif //OBDPLATFORM_POSIX

#ifdef HAVE_GPSD
	// Ping a message to stdout the first time we get
	//   enough of a satellite lock to begin logging
	int have_gps_lock = 0;
#endif //HAVE_GPSD


	install_signalhandlers();

	int nmessage = 0;
	int nrows = 0;
	// The current time we're inserting
	double time_insert;
	// The last time we tried to check the gps daemon
	double time_lastgpscheck = 0;
	int lastRpm = 0;
	int currentRpm = 0;

	pn_message_t * message = NULL;
	pn_messenger_t * messenger;
	pn_data_t * body = NULL;

	// initialize the messenger (client) class and initialize it
	messenger = pn_messenger(NULL);
	pn_messenger_start(messenger);

	// send the "start of trip" message
	message = pn_message();
	pn_message_set_address(message, queueAddress);
	pn_message_set_subject(message, "trip");
	body = pn_message_body(message);
	pn_data_enter(body);
	// set the outer map that'll hold all message content
	pn_data_put_map(body);
	pn_data_enter(body);
	pn_data_put_string(body, pn_bytes(strlen("id"), "id"));
	pn_data_put_uuid(body, *(pn_uuid_t*)&driveid);
	pn_data_exit(body); // exit map
	pn_data_exit(body); // exit body

	pn_messenger_put(messenger, message);
	check(messenger);
	pn_messenger_send(messenger, -1);
	check(messenger);
	pn_message_free(message);
	message = NULL;

	while(true) {
		// if we don't have a current message, make one and set it up to
		// collect rows of readings
		if ( message == NULL ) {
			// create the message to take the first set of telemetry messages
			message = pn_message();
			pn_message_set_address(message, queueAddress);
			pn_message_set_subject(message, "tele");
			body = pn_message_body(message);
			pn_data_enter(body);
			// set the outer map that'll hold all message content
			pn_data_put_map(body);
			pn_data_enter(body);
			pn_data_put_string(body, pn_bytes(strlen("id"), "id"));
			pn_data_put_uuid(body, *(pn_uuid_t*)&driveid);
			// store the list of headers. "h" : list of string
			pn_data_put_string(body, pn_bytes(strlen("h"), "h"));
			pn_data_put_list(body);
			pn_data_enter(body);

			// device time
			pn_data_put_string(body, pn_bytes(3, "dtm"));

			//Supported OBD Command Headers
			for(i=0; i<totalSupportedObdCommands; i++) {
				const char * columnId = obdcmds_mode1[supportedObdCommandsMap[i]].db_column;
				pn_data_put_string(body, pn_bytes(strlen(columnId), (char *)columnId));
			}

#ifdef HAVE_GPSD
			// GPS Reading headers
			pn_data_put_string(body, pn_bytes(3, "lat"));
			pn_data_put_string(body, pn_bytes(3, "lon"));
			pn_data_put_string(body, pn_bytes(3, "alt"));
			pn_data_put_string(body, pn_bytes(3, "crs"));
			pn_data_put_string(body, pn_bytes(3, "spd"));
			pn_data_put_string(body, pn_bytes(3, "gtm"));
#endif
			pn_data_exit(body);

			// prep storing the data. "d" : list (rows) of lists (columns)
			pn_data_put_string(body, pn_bytes(strlen("d"), "d"));
			pn_data_put_list(body);
			pn_data_enter(body);
		}

		struct timeval starttime; // start time through loop
		struct timeval endtime; // end time through loop
		struct timeval selecttime; // =endtime-starttime [for select()]

		if(0 != gettimeofday(&starttime,NULL)) {
			perror("Couldn't gettimeofday");
			break;
		}

		time_insert = (double)starttime.tv_sec+(double)starttime.tv_usec/1000000.0f;

		pn_data_put_list(body);
		pn_data_enter(body);

		// write out the time stamp (seconds epoch w/ milliseconds)
		pn_data_put_double(body, time_insert);

		// Get the OBD data of fill with NULLs if the OBD port isn't available
		for(i=0; i<totalSupportedObdCommands-1; i++) {
			if( obd_serial_port >= 0) {
				float val;
				unsigned int cmdid = obdcmds_mode1[supportedObdCommandsMap[i]].cmdid;
				bool isRpm = cmdid == 0x0C; // Engine RPM
				int numbytes = enable_optimisations?obdcmds_mode1[supportedObdCommandsMap[i]].bytes_returned:0;
				OBDConvFunc conv = obdcmds_mode1[supportedObdCommandsMap[i]].conv;
				enum obd_serial_status obdstatus = getobdvalue(obd_serial_port, cmdid, &val, numbytes, conv);
				if(OBD_SUCCESS == obdstatus) {
					if ( isRpm ) {
						currentRpm = val;
					}
					if(spam_stdout) {
						printf("%s=%f\n", obdcmds_mode1[supportedObdCommandsMap[i]].db_column, val);
					}
					// Add value to the map
					pn_data_put_float(body, val);
				} else if (OBD_ERROR == obdstatus) {
					fprintf(stderr, "Received OBD_ERROR from serial read. Exiting\n");
					receive_exitsignal = 1;
				}
			} else {
				pn_data_put_null(body);
			}
		}

#ifdef HAVE_GPSD
		// Get the GPS data
		double lat,lon,alt,speed,course,gpstime;

		int gpsstatus = -1;
		if(NULL != gpsdata) {
			gpsstatus = getgpsposition(gpsdata, &lat, &lon, &alt, &speed, &course, &gpstime);
		} else {
			if(time_insert - time_lastgpscheck > 10) { // Try again once in a while
				gpsdata = opengps(GPSD_ADDR, GPSD_PORT);
				if(NULL != gpsdata) {
					printf("Delayed connection to gps achieved\n");
				} else {
					// fprintf(stderr, "Delayed connection to gps failed\n");
				}
				time_lastgpscheck = time_insert;
			}
		}
		if(gpsstatus < 0 || NULL == gpsdata) {
			// Nothing yet, write out nulls
			int p;
			for ( p = 0; p < 6; p++) {
				pn_data_put_null(body);
			}
		} else if(gpsstatus >= 0) {
			if(0 == have_gps_lock) {
				fprintf(stderr,"GPS acquisition complete\n");
				have_gps_lock = 1;
			}

			pn_data_put_double(body, lat);
			pn_data_put_double(body, lon);
			pn_data_put_double(body, (gpsstatus>=1?alt:-1000.0));
			pn_data_put_double(body, speed);
			pn_data_put_double(body, course);
			pn_data_put_double(body, gpstime);

			if(spam_stdout) {
				printf("gpspos=%f,%f,%f,%f,%f\n",
				       lat, lon, (gpsstatus>=1?alt:-1000.0), speed, course);
			}
		}
#endif //HAVE_GPSD

		// row is done
		pn_data_exit(body);

		nrows ++;
		// we flush the data out if the
		if ( pn_data_size(body) > maxMsgPayload || (lastRpm > 0 && currentRpm == 0)) {
			printf("message %d sent with %d rows\n", ++nmessage, nrows);
			nrows = 0;

			// table is done
			pn_data_exit(body);
			// map is done
			pn_data_exit(body);

			// send it
			pn_messenger_put(messenger, message);
			check(messenger);
			pn_messenger_send(messenger, -1);
			check(messenger);
			pn_message_free(message);
			message = NULL;

		}
		lastRpm = currentRpm;

		if(0 != gettimeofday(&endtime,NULL)) {
			perror("Couldn't gettimeofday");
			break;
		}

		// Set via the signal handler
		if(receive_exitsignal) {
			break;
		}


		// usleep() not as portable as select()
		if(0 < frametime) {
			selecttime.tv_sec = endtime.tv_sec - starttime.tv_sec;
			if (selecttime.tv_sec != 0) {
				endtime.tv_usec += 1000000*selecttime.tv_sec;
				selecttime.tv_sec = 0;
			}
			selecttime.tv_usec = (frametime) -
			                     (endtime.tv_usec - starttime.tv_usec);
			if(selecttime.tv_usec < 0) {
				selecttime.tv_usec = 1;
			}
			select(0,NULL,NULL,NULL,&selecttime);
		}
	}

	pn_messenger_stop(messenger);
	pn_messenger_free(messenger);

	closeserial(obd_serial_port);
#ifdef HAVE_GPSD
	if(NULL != gpsdata) {
		gps_close(gpsdata);
	}
#endif //HAVE_GPSD
//TODO shutdown client

	if(enable_seriallog) {
		closeseriallog();
	}

	if(NULL != log_columns) free(log_columns);
	if(NULL != queueAddress) free(queueAddress);
	if(NULL != serialport) free(serialport);

	obd_freeConfig(obd_config);
	return 0;
}

#ifdef OBDPLATFORM_POSIX
// *sniff sniff*
// Smells like Stevens.
static int obddaemonise()
{
	int fd;
	pid_t pid = fork();

	switch (pid) {
	case -1:
		perror("Couldn't fork");
		return -1;
	case 0: // child
		break;
	default: // Parent
		exit(0);
	}

	if (setsid() == -1)
		return -1;

	if (chdir("/") == -1)
		return -1;

	close(STDIN_FILENO);
	close(STDOUT_FILENO);
	close(STDERR_FILENO);
	if ((fd = open("/dev/null", O_RDWR, 0)) != -1) {
		dup2(fd, STDIN_FILENO);
		dup2(fd, STDOUT_FILENO);
		dup2(fd, STDERR_FILENO);
	}

	return 0;
}
#endif //OBDPLATFORM_POSIX

void printhelp(const char *argv0)
{
	printf("Usage: %s [params]\n"
	       "   [-s|--serial <" OBD_DEFAULT_SERIALPORT ">]\n"
	       "   [-c|--count <infinite>]\n"
	       "   [-i|--log-columns <" OBD_DEFAULT_COLUMNS ">]\n"
	       "   [-t|--spam-stdout]\n"
	       "   [-p|--capabilities]\n"
	       "   [-o|--enable-optimisations]\n"
	       "   [-u|--output-log <filename>]\n"
#ifdef OBDPLATFORM_POSIX
	       "   [-m|--daemonise]\n"
#endif //OBDPLATFORM_POSIX
	       "   [-b|--baud <number>]\n"
	       "   [-B|--modifybaud <number>]\n"
	       "   [-l|--serial-log <filename>]\n"
	       "   [-a|--samplerate [1]]\n"
//	       "   [-q|--queue <" OBD_DEFAULT_QUEUE ">]\n"
	       "   [-v|--version] [-h|--help]\n", argv0);
}

void printversion()
{
	printf("Version: %i.%i\n", OBDGPSLOGGER_MAJOR_VERSION, OBDGPSLOGGER_MINOR_VERSION);
}


void install_signalhandlers()
{
	// Set up signal handling

#ifdef HAVE_SIGACTION
	struct sigaction sa_new;
	memset(&sa_new, 0, sizeof(sa_new));

	// Exit on ctrl+c or SIGTERM
	sa_new.sa_handler = catch_quitsignal;
	sigemptyset(&sa_new.sa_mask);
	sigaddset(&sa_new.sa_mask, SIGINT);
	sigaddset(&sa_new.sa_mask, SIGTERM);
	sigaction(SIGINT, &sa_new, NULL);
	sigaction(SIGTERM, &sa_new, NULL);

#ifdef SIGUSR1
	// Start a trip on USR1
	sa_new.sa_handler = catch_tripstartsignal;
	sa_new.sa_flags &= ~(SA_RESETHAND);
	sigemptyset(&sa_new.sa_mask);
	sigaddset(&sa_new.sa_mask, SIGUSR1);
	sigaction(SIGUSR1, &sa_new, NULL);
#endif //SIGUSR1

#else // HAVE_SIGACTION

// If your unix implementation doesn't have sigaction, we can fall
//  back to the older [bad, unsafe] signal().
#ifdef HAVE_SIGNAL_FUNC

	// Exit on ctrl+c or TERM
	signal(SIGINT, catch_quitsignal);
	signal(SIGTERM, catch_quitsignal);

#ifdef SIGUSR1
	// Start a trip on USR1
	signal(SIGUSR1, catch_tripstartsignal);
#endif //SIGUSR1

#endif // HAVE_SIGNAL_FUNC


#endif //HAVE_SIGACTION

}
