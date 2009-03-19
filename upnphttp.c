/* MiniDLNA project
 * http://minidlna.sourceforge.net/
 * (c) 2008-2009 Justin Maggard
 *
 * This software is subject to the conditions detailed
 * in the LICENCE file provided within the distribution 
 *
 * Portions of the code from the MiniUPnP Project
 * (c) Thomas Bernard licensed under BSD revised license
 * detailed in the LICENSE.miniupnpd file provided within
 * the distribution.
 */
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/param.h>
#include <ctype.h>
#include "config.h"
#include "upnphttp.h"
#include "upnpdescgen.h"
#include "minidlnapath.h"
#include "upnpsoap.h"
#include "upnpevents.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/sendfile.h>

#include "upnpglobalvars.h"
#include "utils.h"
#include "image_utils.h"
#include "log.h"
#include "sql.h"
#include <libexif/exif-loader.h>
#ifdef TIVO_SUPPORT
#include "tivo_utils.h"
#include "tivo_commands.h"
#endif
//#define MAX_BUFFER_SIZE 4194304 // 4MB -- Too much?
#define MAX_BUFFER_SIZE 2147483647 // 2GB -- Too much?

#include "icons.c"

struct upnphttp * 
New_upnphttp(int s)
{
	struct upnphttp * ret;
	if(s<0)
		return NULL;
	ret = (struct upnphttp *)malloc(sizeof(struct upnphttp));
	if(ret == NULL)
		return NULL;
	memset(ret, 0, sizeof(struct upnphttp));
	ret->socket = s;
	return ret;
}

void
CloseSocket_upnphttp(struct upnphttp * h)
{
	if(close(h->socket) < 0)
	{
		DPRINTF(E_ERROR, L_HTTP, "CloseSocket_upnphttp: close(%d): %s\n", h->socket, strerror(errno));
	}
	h->socket = -1;
	h->state = 100;
}

void
Delete_upnphttp(struct upnphttp * h)
{
	if(h)
	{
		if(h->socket >= 0)
			CloseSocket_upnphttp(h);
		if(h->req_buf)
			free(h->req_buf);
		if(h->res_buf)
			free(h->res_buf);
		free(h);
	}
}

/* parse HttpHeaders of the REQUEST */
static void
ParseHttpHeaders(struct upnphttp * h)
{
	char * line;
	char * colon;
	char * p;
	int n;
	line = h->req_buf;
	/* TODO : check if req_buf, contentoff are ok */
	while(line < (h->req_buf + h->req_contentoff))
	{
		colon = strchr(line, ':');
		if(colon)
		{
			if(strncasecmp(line, "Content-Length", 14)==0)
			{
				p = colon;
				while(*p < '0' || *p > '9')
					p++;
				h->req_contentlen = atoi(p);
			}
			else if(strncasecmp(line, "SOAPAction", 10)==0)
			{
				p = colon;
				n = 0;
				while(*p == ':' || *p == ' ' || *p == '\t')
					p++;
				while(p[n]>=' ')
				{
					n++;
				}
				if((p[0] == '"' && p[n-1] == '"')
				  || (p[0] == '\'' && p[n-1] == '\''))
				{
					p++; n -= 2;
				}
				h->req_soapAction = p;
				h->req_soapActionLen = n;
			}
			else if(strncasecmp(line, "Callback", 8)==0)
			{
				p = colon;
				while(*p != '<' && *p != '\r' )
					p++;
				n = 0;
				while(p[n] != '>' && p[n] != '\r' )
					n++;
				h->req_Callback = p + 1;
				h->req_CallbackLen = MAX(0, n - 1);
			}
			else if(strncasecmp(line, "SID", 3)==0)
			{
				p = colon + 1;
				while(isspace(*p))
					p++;
				n = 0;
				while(!isspace(p[n]))
					n++;
				h->req_SID = p;
				h->req_SIDLen = n;
			}
			/* Timeout: Seconds-nnnn */
/* TIMEOUT
Recommended. Requested duration until subscription expires,
either number of seconds or infinite. Recommendation
by a UPnP Forum working committee. Defined by UPnP vendor.
 Consists of the keyword "Second-" followed (without an
intervening space) by either an integer or the keyword "infinite". */
			else if(strncasecmp(line, "Timeout", 7)==0)
			{
				p = colon + 1;
				while(isspace(*p))
					p++;
				if(strncasecmp(p, "Second-", 7)==0) {
					h->req_Timeout = atoi(p+7);
				}
			}
			// Range: bytes=xxx-yyy
			else if(strncasecmp(line, "Range", 5)==0)
			{
				p = colon + 1;
				while(isspace(*p))
					p++;
				if(strncasecmp(p, "bytes=", 6)==0) {
					h->reqflags |= FLAG_RANGE;
					h->req_RangeEnd = atoll(index(p+6, '-')+1);
					h->req_RangeStart = atoll(p+6);
					DPRINTF(E_DEBUG, L_HTTP, "Range Start-End: %lld - %lld\n",
					       h->req_RangeStart, h->req_RangeEnd?h->req_RangeEnd:-1);
				}
			}
			else if(strncasecmp(line, "Host", 4)==0)
			{
				h->reqflags |= FLAG_HOST;
			}
			else if(strncasecmp(line, "User-Agent", 10)==0)
			{
				p = colon + 1;
				while(isspace(*p))
					p++;
				if(strncasecmp(p, "Xbox/", 5)==0)
				{
					h->req_client = EXbox;
				}
			}
			else if(strncasecmp(line, "Transfer-Encoding", 17)==0)
			{
				p = colon + 1;
				while(isspace(*p))
					p++;
				if(strncasecmp(p, "chunked", 7)==0)
				{
					h->reqflags |= FLAG_CHUNKED;
				}
			}
			else if(strncasecmp(line, "getcontentFeatures.dlna.org", 27)==0)
			{
				p = colon + 1;
				while(isspace(*p))
					p++;
				if( (*p != '1') || !isspace(p[1]) )
					h->reqflags |= FLAG_INVALID_REQ;
			}
			else if(strncasecmp(line, "TimeSeekRange.dlna.org", 22)==0)
			{
				h->reqflags |= FLAG_TIMESEEK;
			}
			else if(strncasecmp(line, "realTimeInfo.dlna.org", 21)==0)
			{
				h->reqflags |= FLAG_REALTIMEINFO;
			}
			else if(strncasecmp(line, "transferMode.dlna.org", 21)==0)
			{
				p = colon + 1;
				while(isspace(*p))
					p++;
				if(strncasecmp(p, "Streaming", 9)==0)
				{
					h->reqflags |= FLAG_XFERSTREAMING;
				}
				if(strncasecmp(p, "Interactive", 11)==0)
				{
					h->reqflags |= FLAG_XFERINTERACTIVE;
				}
				if(strncasecmp(p, "Background", 10)==0)
				{
					h->reqflags |= FLAG_XFERBACKGROUND;
				}
			}
		}
		while(!(line[0] == '\r' && line[1] == '\n'))
			line++;
		line += 2;
	}
	if( h->reqflags & FLAG_CHUNKED )
	{
		if( h->req_buflen > h->req_contentoff )
		{
			h->req_chunklen = strtol(line, NULL, 16);
			while(!(line[0] == '\r' && line[1] == '\n'))
			{
				line++;
				h->req_contentoff++;
			}
			h->req_contentoff += 2;
		}
		else
		{
			h->req_chunklen = -1;
		}
	}
}

/* very minimalistic 400 error message */
static void
Send400(struct upnphttp * h)
{
	static const char body400[] =
		"<HTML><HEAD><TITLE>400 Bad Request</TITLE></HEAD>"
		"<BODY><H1>Bad Request</H1>The request is invalid"
		" for this HTTP version.</BODY></HTML>\r\n";
	h->respflags = FLAG_HTML;
	BuildResp2_upnphttp(h, 400, "Bad Request",
	                    body400, sizeof(body400) - 1);
	SendResp_upnphttp(h);
	CloseSocket_upnphttp(h);
}

/* very minimalistic 404 error message */
static void
Send404(struct upnphttp * h)
{
	static const char body404[] =
		"<HTML><HEAD><TITLE>404 Not Found</TITLE></HEAD>"
		"<BODY><H1>Not Found</H1>The requested URL was not found"
		" on this server.</BODY></HTML>\r\n";
	h->respflags = FLAG_HTML;
	BuildResp2_upnphttp(h, 404, "Not Found",
	                    body404, sizeof(body404) - 1);
	SendResp_upnphttp(h);
	CloseSocket_upnphttp(h);
}

/* very minimalistic 406 error message */
static void
Send406(struct upnphttp * h)
{
	static const char body406[] =
		"<HTML><HEAD><TITLE>406 Not Acceptable</TITLE></HEAD>"
		"<BODY><H1>Not Acceptable</H1>An unsupported operation "
		" was requested.</BODY></HTML>\r\n";
	h->respflags = FLAG_HTML;
	BuildResp2_upnphttp(h, 406, "Not Acceptable",
	                    body406, sizeof(body406) - 1);
	SendResp_upnphttp(h);
	CloseSocket_upnphttp(h);
}

/* very minimalistic 416 error message */
static void
Send416(struct upnphttp * h)
{
	static const char body416[] =
		"<HTML><HEAD><TITLE>416 Requested Range Not Satisfiable</TITLE></HEAD>"
		"<BODY><H1>Requested Range Not Satisfiable</H1>The requested range"
		" was outside the file's size.</BODY></HTML>\r\n";
	h->respflags = FLAG_HTML;
	BuildResp2_upnphttp(h, 416, "Requested Range Not Satisfiable",
	                    body416, sizeof(body416) - 1);
	SendResp_upnphttp(h);
	CloseSocket_upnphttp(h);
}

/* very minimalistic 501 error message */
static void
Send501(struct upnphttp * h)
{
	static const char body501[] = 
		"<HTML><HEAD><TITLE>501 Not Implemented</TITLE></HEAD>"
		"<BODY><H1>Not Implemented</H1>The HTTP Method "
		"is not implemented by this server.</BODY></HTML>\r\n";
	h->respflags = FLAG_HTML;
	BuildResp2_upnphttp(h, 501, "Not Implemented",
	                    body501, sizeof(body501) - 1);
	SendResp_upnphttp(h);
	CloseSocket_upnphttp(h);
}

static const char *
findendheaders(const char * s, int len)
{
	while(len-->0)
	{
		if(s[0]=='\r' && s[1]=='\n' && s[2]=='\r' && s[3]=='\n')
			return s;
		s++;
	}
	return NULL;
}

/* Sends the description generated by the parameter */
static void
sendXMLdesc(struct upnphttp * h, char * (f)(int *))
{
	char * desc;
	int len;
	desc = f(&len);
	if(!desc)
	{
		static const char error500[] = "<HTML><HEAD><TITLE>Error 500</TITLE>"
		   "</HEAD><BODY>Internal Server Error</BODY></HTML>\r\n";
		DPRINTF(E_ERROR, L_HTTP, "Failed to generate XML description\n");
		h->respflags = FLAG_HTML;
		BuildResp2_upnphttp(h, 500, "Internal Server Error",
		                    error500, sizeof(error500)-1);
	}
	else
	{
		BuildResp_upnphttp(h, desc, len);
	}
	SendResp_upnphttp(h);
	CloseSocket_upnphttp(h);
	free(desc);
}

/* ProcessHTTPPOST_upnphttp()
 * executes the SOAP query if it is possible */
static void
ProcessHTTPPOST_upnphttp(struct upnphttp * h)
{
	if((h->req_buflen - h->req_contentoff) >= h->req_contentlen)
	{
		if(h->req_soapAction)
		{
			/* we can process the request */
			DPRINTF(E_DEBUG, L_HTTP, "SOAPAction: %.*s\n", h->req_soapActionLen, h->req_soapAction);
			ExecuteSoapAction(h, 
				h->req_soapAction,
				h->req_soapActionLen);
		}
		else
		{
			static const char err400str[] =
				"<html><body>Bad request</body></html>";
			DPRINTF(E_WARN, L_HTTP, "No SOAPAction in HTTP headers");
			h->respflags = FLAG_HTML;
			BuildResp2_upnphttp(h, 400, "Bad Request",
			                    err400str, sizeof(err400str) - 1);
			SendResp_upnphttp(h);
			CloseSocket_upnphttp(h);
		}
	}
	else
	{
		/* waiting for remaining data */
		h->state = 1;
	}
}

static void
ProcessHTTPSubscribe_upnphttp(struct upnphttp * h, const char * path)
{
	const char * sid;
	DPRINTF(E_DEBUG, L_HTTP, "ProcessHTTPSubscribe %s\n", path);
	DPRINTF(E_DEBUG, L_HTTP, "Callback '%.*s' Timeout=%d\n",
	       h->req_CallbackLen, h->req_Callback, h->req_Timeout);
	DPRINTF(E_DEBUG, L_HTTP, "SID '%.*s'\n", h->req_SIDLen, h->req_SID);
	if(!h->req_Callback && !h->req_SID) {
		/* Missing or invalid CALLBACK : 412 Precondition Failed.
		 * If CALLBACK header is missing or does not contain a valid HTTP URL,
		 * the publisher must respond with HTTP error 412 Precondition Failed*/
		BuildResp2_upnphttp(h, 412, "Precondition Failed", 0, 0);
		SendResp_upnphttp(h);
		CloseSocket_upnphttp(h);
	} else {
	/* - add to the subscriber list
	 * - respond HTTP/x.x 200 OK 
	 * - Send the initial event message */
/* Server:, SID:; Timeout: Second-(xx|infinite) */
		if(h->req_Callback) {
			sid = upnpevents_addSubscriber(path, h->req_Callback,
			                               h->req_CallbackLen, h->req_Timeout);
			h->respflags = FLAG_TIMEOUT;
			if(sid) {
				DPRINTF(E_DEBUG, L_HTTP, "generated sid=%s\n", sid);
				h->respflags |= FLAG_SID;
				h->req_SID = sid;
				h->req_SIDLen = strlen(sid);
			}
			BuildResp_upnphttp(h, 0, 0);
		} else {
			/* subscription renew */
			/* Invalid SID
412 Precondition Failed. If a SID does not correspond to a known,
un-expired subscription, the publisher must respond
with HTTP error 412 Precondition Failed. */
			if(renewSubscription(h->req_SID, h->req_SIDLen, h->req_Timeout) < 0) {
				BuildResp2_upnphttp(h, 412, "Precondition Failed", 0, 0);
			} else {
				/* A DLNA device must enforce a 5 minute timeout */
				h->respflags = FLAG_TIMEOUT;
				h->req_Timeout = 300;
				BuildResp_upnphttp(h, 0, 0);
			}
		}
		SendResp_upnphttp(h);
		CloseSocket_upnphttp(h);
	}
}

static void
ProcessHTTPUnSubscribe_upnphttp(struct upnphttp * h, const char * path)
{
	DPRINTF(E_DEBUG, L_HTTP, "ProcessHTTPUnSubscribe %s\n", path);
	DPRINTF(E_DEBUG, L_HTTP, "SID '%.*s'\n", h->req_SIDLen, h->req_SID);
	/* Remove from the list */
	if(upnpevents_removeSubscriber(h->req_SID, h->req_SIDLen) < 0) {
		BuildResp2_upnphttp(h, 412, "Precondition Failed", 0, 0);
	} else {
		BuildResp_upnphttp(h, 0, 0);
	}
	SendResp_upnphttp(h);
	CloseSocket_upnphttp(h);
}

/* Parse and process Http Query 
 * called once all the HTTP headers have been received. */
static void
ProcessHttpQuery_upnphttp(struct upnphttp * h)
{
	char HttpCommand[16];
	char HttpUrl[512];
	char * HttpVer;
	char * p;
	int i;
	p = h->req_buf;
	if(!p)
		return;
	for(i = 0; i<15 && *p != ' ' && *p != '\r'; i++)
		HttpCommand[i] = *(p++);
	HttpCommand[i] = '\0';
	while(*p==' ')
		p++;
	if(strncmp(p, "http://", 7) == 0)
	{
		p = p+7;
		while(*p!='/')
			p++;
	}
	for(i = 0; i<511 && *p != ' ' && *p != '\r'; i++)
		HttpUrl[i] = *(p++);
	HttpUrl[i] = '\0';
	while(*p==' ')
		p++;
	HttpVer = h->HttpVer;
	for(i = 0; i<15 && *p != '\r'; i++)
		HttpVer[i] = *(p++);
	HttpVer[i] = '\0';
	/*DPRINTF(E_INFO, L_HTTP, "HTTP REQUEST : %s %s (%s)\n",
	       HttpCommand, HttpUrl, HttpVer);*/
	DPRINTF(E_DEBUG, L_HTTP, "HTTP REQUEST: %.*s\n", h->req_buflen, h->req_buf);
	ParseHttpHeaders(h);

	/* see if we need to wait for remaining data */
	if( (h->reqflags & FLAG_CHUNKED) )
	{
		char * chunkstart = h->req_buf+h->req_contentoff;
		char * numstart;
		h->state = 2;
		while( h->req_chunklen )
		{
			if( chunkstart >= (h->req_buf+h->req_buflen) )
				return;
			numstart = chunkstart+h->req_chunklen+2;
			h->req_chunklen = strtol(numstart, &chunkstart, 16);
			if( !h->req_chunklen && (chunkstart == numstart) )
			{
				DPRINTF(E_DEBUG, L_HTTP, "Chunked request needs more input.\n");
				return;
			}
			chunkstart = chunkstart+2;
		}
		h->state = 100;
	}
	if(strcmp("POST", HttpCommand) == 0)
	{
		h->req_command = EPost;
		ProcessHTTPPOST_upnphttp(h);
	}
	else if((strcmp("GET", HttpCommand) == 0) || (strcmp("HEAD", HttpCommand) == 0))
	{
		if( ((strcmp(h->HttpVer, "HTTP/1.1")==0) && !(h->reqflags & FLAG_HOST)) || (h->reqflags & FLAG_INVALID_REQ) )
		{
			DPRINTF(E_WARN, L_HTTP, "Invalid request, responding ERROR 400.  (No Host specified in HTTP headers?)\n");
			Send400(h);
		}
		else if( h->reqflags & FLAG_TIMESEEK )
		{
			DPRINTF(E_WARN, L_HTTP, "DLNA TimeSeek requested, responding ERROR 406\n");
			Send406(h);
		}
		else if(strcmp("GET", HttpCommand) == 0)
		{
			h->req_command = EGet;
		}
		else
		{
			h->req_command = EHead;
		}
		if(strcmp(ROOTDESC_PATH, HttpUrl) == 0)
		{
			sendXMLdesc(h, genRootDesc);
		}
		else if(strcmp(CONTENTDIRECTORY_PATH, HttpUrl) == 0)
		{
			sendXMLdesc(h, genContentDirectory);
		}
		else if(strcmp(CONNECTIONMGR_PATH, HttpUrl) == 0)
		{
			sendXMLdesc(h, genConnectionManager);
		}
		else if(strcmp(X_MS_MEDIARECEIVERREGISTRAR_PATH, HttpUrl) == 0)
		{
			sendXMLdesc(h, genX_MS_MediaReceiverRegistrar);
		}
		else if(strncmp(HttpUrl, "/MediaItems/", 12) == 0)
		{
			SendResp_dlnafile(h, HttpUrl+12);
			CloseSocket_upnphttp(h);
		}
		else if(strncmp(HttpUrl, "/Thumbnails/", 12) == 0)
		{
			SendResp_thumbnail(h, HttpUrl+12);
			CloseSocket_upnphttp(h);
		}
		else if(strncmp(HttpUrl, "/AlbumArt/", 10) == 0)
		{
			SendResp_albumArt(h, HttpUrl+10);
			CloseSocket_upnphttp(h);
		}
		#ifdef TIVO_SUPPORT
		else if(strncmp(HttpUrl, "/TiVoConnect", 12) == 0)
		{
			if( GETFLAG(TIVOMASK) )
			{
				if( *(HttpUrl+12) == '?' )
				{
					ProcessTiVoCommand(h, HttpUrl+13);
				}
				else
				{
					printf("Invalid TiVo request! %s\n", HttpUrl+12);
					Send404(h);
				}
			}
			else
			{
				printf("TiVo request with out TiVo support enabled! %s\n", HttpUrl+12);
				Send404(h);
			}
		}
		#endif
		else if(strncmp(HttpUrl, "/Resized/", 9) == 0)
		{
			SendResp_resizedimg(h, HttpUrl+9);
			CloseSocket_upnphttp(h);
		}
		else if(strncmp(HttpUrl, "/icons/", 7) == 0)
		{
			SendResp_icon(h, HttpUrl+7);
			CloseSocket_upnphttp(h);
		}
		else
		{
			DPRINTF(E_WARN, L_HTTP, "%s not found, responding ERROR 404\n", HttpUrl);
			Send404(h);
		}
	}
	else if(strcmp("SUBSCRIBE", HttpCommand) == 0)
	{
		h->req_command = ESubscribe;
		ProcessHTTPSubscribe_upnphttp(h, HttpUrl);
	}
	else if(strcmp("UNSUBSCRIBE", HttpCommand) == 0)
	{
		h->req_command = EUnSubscribe;
		ProcessHTTPUnSubscribe_upnphttp(h, HttpUrl);
	}
	else
	{
		DPRINTF(E_WARN, L_HTTP, "Unsupported HTTP Command %s\n", HttpCommand);
		Send501(h);
	}
}


void
Process_upnphttp(struct upnphttp * h)
{
	char buf[2048];
	int n;
	if(!h)
		return;
	switch(h->state)
	{
	case 0:
		n = recv(h->socket, buf, 2048, 0);
		if(n<0)
		{
			DPRINTF(E_ERROR, L_HTTP, "recv (state0): %s\n", strerror(errno));
			h->state = 100;
		}
		else if(n==0)
		{
			DPRINTF(E_WARN, L_HTTP, "HTTP Connection closed inexpectedly\n");
			h->state = 100;
		}
		else
		{
			const char * endheaders;
			/* if 1st arg of realloc() is null,
			 * realloc behaves the same as malloc() */
			h->req_buf = (char *)realloc(h->req_buf, n + h->req_buflen + 1);
			memcpy(h->req_buf + h->req_buflen, buf, n);
			h->req_buflen += n;
			h->req_buf[h->req_buflen] = '\0';
			/* search for the string "\r\n\r\n" */
			endheaders = findendheaders(h->req_buf, h->req_buflen);
			if(endheaders)
			{
				h->req_contentoff = endheaders - h->req_buf + 4;
				ProcessHttpQuery_upnphttp(h);
			}
		}
		break;
	case 1:
	case 2:
		n = recv(h->socket, buf, 2048, 0);
		if(n<0)
		{
			DPRINTF(E_ERROR, L_HTTP, "recv (state1): %s\n", strerror(errno));
			h->state = 100;
		}
		else if(n==0)
		{
			DPRINTF(E_WARN, L_HTTP, "HTTP Connection closed inexpectedly\n");
			h->state = 100;
		}
		else
		{
			/*fwrite(buf, 1, n, stdout);*/	/* debug */
			h->req_buf = (char *)realloc(h->req_buf, n + h->req_buflen);
			memcpy(h->req_buf + h->req_buflen, buf, n);
			h->req_buflen += n;
			if((h->req_buflen - h->req_contentoff) >= h->req_contentlen)
			{
				if( h->state == 1 )
					ProcessHTTPPOST_upnphttp(h);
				else if( h->state == 2 )
					ProcessHttpQuery_upnphttp(h);
			}
		}
		break;
	default:
		DPRINTF(E_WARN, L_HTTP, "Unexpected state: %d\n", h->state);
	}
}

static const char httpresphead[] =
	"%s %d %s\r\n"
	/*"Content-Type: text/xml; charset=\"utf-8\"\r\n"*/
	"Content-Type: %s\r\n"
	"Connection: close\r\n"
	"Content-Length: %d\r\n"
	/*"Server: miniupnpd/1.0 UPnP/1.0\r\n"*/
//	"Accept-Ranges: bytes\r\n"
	//"Server: " MINIUPNPD_SERVER_STRING "\r\n"
	;	/*"\r\n";*/
/*
		"<?xml version=\"1.0\"?>\n"
		"<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
		"s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
		"<s:Body>"

		"</s:Body>"
		"</s:Envelope>";
*/
/* with response code and response message
 * also allocate enough memory */

void
BuildHeader_upnphttp(struct upnphttp * h, int respcode,
                     const char * respmsg,
                     int bodylen)
{
	int templen;
	if(!h->res_buf)
	{
		templen = sizeof(httpresphead) + 128 + bodylen;
		h->res_buf = (char *)malloc(templen);
		h->res_buf_alloclen = templen;
	}
	h->res_buflen = snprintf(h->res_buf, h->res_buf_alloclen,
	                         //httpresphead, h->HttpVer,
	                         httpresphead, "HTTP/1.1",
	                         respcode, respmsg,
	                         (h->respflags&FLAG_HTML)?"text/html":"text/xml; charset=\"utf-8\"",
							 bodylen);
	/* Additional headers */
	if(h->respflags & FLAG_TIMEOUT) {
		h->res_buflen += snprintf(h->res_buf + h->res_buflen,
		                          h->res_buf_alloclen - h->res_buflen,
		                          "Timeout: Second-");
		if(h->req_Timeout) {
			h->res_buflen += snprintf(h->res_buf + h->res_buflen,
			                          h->res_buf_alloclen - h->res_buflen,
			                          "%d\r\n", h->req_Timeout);
		} else {
			h->res_buflen += snprintf(h->res_buf + h->res_buflen,
			                          h->res_buf_alloclen - h->res_buflen,
			                          "300\r\n");
			                          //JM DLNA must force to 300 - "infinite\r\n");
		}
	}
	if(h->respflags & FLAG_SID) {
		h->res_buflen += snprintf(h->res_buf + h->res_buflen,
		                          h->res_buf_alloclen - h->res_buflen,
		                          "SID: %s\r\n", h->req_SID);
	}
#if 0 // DLNA
	h->res_buflen += snprintf(h->res_buf + h->res_buflen,
	                          h->res_buf_alloclen - h->res_buflen,
	                          "Server: Microsoft-Windows-NT/5.1 UPnP/1.0 UPnP-Device-Host/1.0\r\n");
	char   szTime[30];
	time_t curtime = time(NULL);
	strftime(szTime, 30,"%a, %d %b %Y %H:%M:%S GMT" , gmtime(&curtime));
	h->res_buflen += snprintf(h->res_buf + h->res_buflen,
	                          h->res_buf_alloclen - h->res_buflen,
	                          "Date: %s\r\n", szTime);
//	h->res_buflen += snprintf(h->res_buf + h->res_buflen,
//	                          h->res_buf_alloclen - h->res_buflen,
//	                          "contentFeatures.dlna.org: \r\n");
//	h->res_buflen += snprintf(h->res_buf + h->res_buflen,
//	                          h->res_buf_alloclen - h->res_buflen,
//	                          "EXT:\r\n");
#endif
	h->res_buf[h->res_buflen++] = '\r';
	h->res_buf[h->res_buflen++] = '\n';
	if(h->res_buf_alloclen < (h->res_buflen + bodylen))
	{
		h->res_buf = (char *)realloc(h->res_buf, (h->res_buflen + bodylen));
		h->res_buf_alloclen = h->res_buflen + bodylen;
	}
}

void
BuildResp2_upnphttp(struct upnphttp * h, int respcode,
                    const char * respmsg,
                    const char * body, int bodylen)
{
	BuildHeader_upnphttp(h, respcode, respmsg, bodylen);
	if( h->req_command == EHead )
		return;
	if(body)
		memcpy(h->res_buf + h->res_buflen, body, bodylen);
	h->res_buflen += bodylen;
}

/* responding 200 OK ! */
void
BuildResp_upnphttp(struct upnphttp * h,
                        const char * body, int bodylen)
{
	BuildResp2_upnphttp(h, 200, "OK", body, bodylen);
}

void
SendResp_upnphttp(struct upnphttp * h)
{
	int n;
	DPRINTF(E_DEBUG, L_HTTP, "HTTP RESPONSE: %.*s\n", h->res_buflen, h->res_buf);
	n = send(h->socket, h->res_buf, h->res_buflen, 0);
	if(n<0)
	{
		DPRINTF(E_ERROR, L_HTTP, "send(res_buf): %s", strerror(errno));
	}
	else if(n < h->res_buflen)
	{
		/* TODO : handle correctly this case */
		DPRINTF(E_ERROR, L_HTTP, "send(res_buf): %d bytes sent (out of %d)\n",
						n, h->res_buflen);
	}
}

int
send_data(struct upnphttp * h, char * header, size_t size)
{
	int n;

	n = send(h->socket, header, size, 0);
	if(n<0)
	{
		DPRINTF(E_ERROR, L_HTTP, "send(res_buf): %s", strerror(errno));
	} 
	else if(n < h->res_buflen)
	{
		/* TODO : handle correctly this case */
		DPRINTF(E_ERROR, L_HTTP, "send(res_buf): %d bytes sent (out of %d)\n",
						n, h->res_buflen);
	}
	else
	{
		return 0;
	}
	return 1;
}

void
send_file(struct upnphttp * h, int sendfd, off_t offset, off_t end_offset)
{
	off_t send_size;
	off_t ret;

	while( offset < end_offset )
	{
		send_size = ( ((end_offset - offset) < MAX_BUFFER_SIZE) ? (end_offset - offset + 1) : MAX_BUFFER_SIZE);
		ret = sendfile(h->socket, sendfd, &offset, send_size);
		if( ret == -1 )
		{
			DPRINTF(E_WARN, L_HTTP, "sendfile error :: error no. %d [%s]\n", errno, strerror(errno));
			if( errno != EAGAIN )
				break;
		}
		/*else
		{
			DPRINTF(E_DEBUG, L_HTTP, "sent %lld bytes to %d. offset is now %lld.\n", ret, h->socket, offset);
		}*/
	}
}

void
SendResp_icon(struct upnphttp * h, char * icon)
{
	char * header;
	char * data;
	int size;
	char mime[12];
	char date[30];
	time_t curtime = time(NULL);

	if( strcmp(icon, "sm.png") == 0 )
	{
		DPRINTF(E_DEBUG, L_HTTP, "Sending small PNG icon\n");
		data = (char *)png_sm;
		size = sizeof(png_sm)-1;
		strcpy(mime, "image/png");
	}
	else if( strcmp(icon, "lrg.png") == 0 )
	{
		DPRINTF(E_DEBUG, L_HTTP, "Sending large PNG icon\n");
		data = (char *)png_lrg;
		size = sizeof(png_lrg)-1;
		strcpy(mime, "image/png");
	}
	else if( strcmp(icon, "sm.jpg") == 0 )
	{
		DPRINTF(E_DEBUG, L_HTTP, "Sending small JPEG icon\n");
		data = (char *)jpeg_sm;
		size = sizeof(jpeg_sm)-1;
		strcpy(mime, "image/jpeg");
	}
	else if( strcmp(icon, "lrg.jpg") == 0 )
	{
		DPRINTF(E_DEBUG, L_HTTP, "Sending large JPEG icon\n");
		data = (char *)jpeg_lrg;
		size = sizeof(jpeg_lrg)-1;
		strcpy(mime, "image/jpeg");
	}
	else
	{
		DPRINTF(E_WARN, L_HTTP, "Invalid icon request: %s\n", icon);
		Send404(h);
		return;
	}


	strftime(date, 30,"%a, %d %b %Y %H:%M:%S GMT" , gmtime(&curtime));
	asprintf(&header, "HTTP/1.1 200 OK\r\n"
			  "Content-Type: %s\r\n"
			  "Content-Length: %d\r\n"
			  "Connection: close\r\n"
			  "Date: %s\r\n"
			  "Server: RAIDiator/4.1, UPnP/1.0, MiniDLNA/1.0\r\n\r\n",
			  mime, size, date);

	if( (send_data(h, header, strlen(header)) == 0) && (h->req_command != EHead) )
	{
		send_data(h, data, size);
	}
	free(header);
}


void
SendResp_albumArt(struct upnphttp * h, char * object)
{
	char header[1500];
	char sql_buf[256];
	char **result;
	int rows;
	char *path;
	char date[30];
	time_t curtime = time(NULL);
	off_t offset = 0, size;
	int sendfh;

	memset(header, 0, 1500);

	if( h->reqflags & FLAG_XFERSTREAMING || h->reqflags & FLAG_RANGE )
	{
		DPRINTF(E_WARN, L_HTTP, "Client tried to specify transferMode as Streaming with an image!\n");
		Send406(h);
		return;
	}

	strip_ext(object);
	sprintf(sql_buf, "SELECT PATH from ALBUM_ART where ID = %s", object);
	sqlite3_get_table(db, sql_buf, &result, &rows, 0, 0);
	if( !rows )
	{
		DPRINTF(E_WARN, L_HTTP, "ALBUM_ART ID %s not found, responding ERROR 404\n", object);
		Send404(h);
		goto error;
	}
	path = result[1];
	DPRINTF(E_INFO, L_HTTP, "Serving album art ID: %s [%s]\n", object, path);

	if( access(path, F_OK) == 0 )
	{
		strftime(date, 30,"%a, %d %b %Y %H:%M:%S GMT" , gmtime(&curtime));

		sendfh = open(path, O_RDONLY);
		if( sendfh < 0 ) {
			DPRINTF(E_ERROR, L_HTTP, "Error opening %s\n", path);
			goto error;
		}
		size = lseek(sendfh, 0, SEEK_END);
		lseek(sendfh, 0, SEEK_SET);

		sprintf(header, "HTTP/1.1 200 OK\r\n"
				"Content-Type: image/jpeg\r\n"
				"Content-Length: %lld\r\n"
				"Connection: close\r\n"
				"Date: %s\r\n"
				"EXT:\r\n"
			 	"contentFeatures.dlna.org: DLNA.ORG_PN=JPEG_TN\r\n"
				"Server: RAIDiator/4.1, UPnP/1.0, MiniDLNA/1.0\r\n",
				size, date);

		if( h->reqflags & FLAG_XFERBACKGROUND )
		{
			strcat(header, "transferMode.dlna.org: Background\r\n\r\n");
		}
		else //if( h->reqflags & FLAG_XFERINTERACTIVE )
		{
			strcat(header, "transferMode.dlna.org: Interactive\r\n\r\n");
		}


		if( (send_data(h, header, strlen(header)) == 0) && (h->req_command != EHead) && (sendfh > 0) )
		{
			send_file(h, sendfh, offset, size);
		}
		close(sendfh);
	}
	error:
	sqlite3_free_table(result);
}

void
SendResp_thumbnail(struct upnphttp * h, char * object)
{
	char header[1500];
	char sql_buf[256];
	char **result;
	int rows;
	char *path;
	char date[30];
	time_t curtime = time(NULL);
	ExifData *ed;
	ExifLoader *l;

	memset(header, 0, 1500);

	if( h->reqflags & FLAG_XFERSTREAMING || h->reqflags & FLAG_RANGE )
	{
		DPRINTF(E_WARN, L_HTTP, "Client tried to specify transferMode as Streaming with an image!\n");
		Send406(h);
		return;
	}

	strip_ext(object);
	sprintf(sql_buf, "SELECT PATH from DETAILS where ID = '%s'", object);
	sqlite3_get_table(db, sql_buf, &result, &rows, 0, 0);
	if( !rows )
	{
		DPRINTF(E_WARN, L_HTTP, "%s not found, responding ERROR 404\n", object);
		Send404(h);
		goto error;
	}
	path = result[1];
	DPRINTF(E_INFO, L_HTTP, "Serving thumbnail for ObjectId: %s [%s]\n", object, path);

	if( access(path, F_OK) == 0 )
	{
		strftime(date, 30,"%a, %d %b %Y %H:%M:%S GMT" , gmtime(&curtime));

		l = exif_loader_new();
		exif_loader_write_file(l, path);
		ed = exif_loader_get_data(l);
		exif_loader_unref(l);

		if( !ed || !ed->size )
		{
			Send404(h);
			if( ed )
				exif_data_unref(ed);
			goto error;
		}
		sprintf(header, "HTTP/1.1 200 OK\r\n"
				"Content-Type: image/jpeg\r\n"
				"Content-Length: %d\r\n"
				"Connection: close\r\n"
				"Date: %s\r\n"
				"EXT:\r\n"
			 	"contentFeatures.dlna.org: DLNA.ORG_PN=JPEG_TN\r\n"
				"Server: RAIDiator/4.1, UPnP/1.0, MiniDLNA/1.0\r\n",
				ed->size, date);

		if( h->reqflags & FLAG_XFERBACKGROUND )
		{
			strcat(header, "transferMode.dlna.org: Background\r\n\r\n");
		}
		else //if( h->reqflags & FLAG_XFERINTERACTIVE )
		{
			strcat(header, "transferMode.dlna.org: Interactive\r\n\r\n");
		}

		if( (send_data(h, header, strlen(header)) == 0) && (h->req_command != EHead) )
		{
			send_data(h, (char *)ed->data, ed->size);
		}
		exif_data_unref(ed);
	}
	error:
	sqlite3_free_table(result);
}

void
SendResp_resizedimg(struct upnphttp * h, char * object)
{
	char header[1500];
	char sql_buf[256];
	char **result;
	char date[30];
	time_t curtime = time(NULL);
	int width=640, height=480, dstw, dsth, rotation, size;
	unsigned char * data;
	char *path, *file_path;
	char *resolution, *tn;
	char *key, *val;
	char *saveptr, *item = NULL;
	char *pixelshape = NULL;
	int rows=0, ret;
	ExifData *ed;
	ExifLoader *l;
	image * imsrc;
	image * imdst;
#if USE_FORK
	pid_t newpid = 0;
	newpid = fork();
	if( newpid )
		return;
#endif
	memset(header, 0, 1500);

	path = strdup(object);
	if( strtok_r(path, "?", &saveptr) )
	{
		item = strtok_r(NULL, "&", &saveptr);
	}
	while( item != NULL )
	{
		#ifdef TIVO_SUPPORT
		decodeString(item, 1);
		#endif
		val = item;
		key = strsep(&val, "=");
		DPRINTF(E_DEBUG, L_GENERAL, "%s: %s\n", key, val);
		if( strcasecmp(key, "width") == 0 )
		{
			width = atoi(val);
		}
		else if( strcasecmp(key, "height") == 0 )
		{
			height = atoi(val);
		}
		else if( strcasecmp(key, "rotation") == 0 )
		{
			rotation = atoi(val);
		}
		else if( strcasecmp(key, "pixelshape") == 0 )
		{
			pixelshape = val;
		}
		item = strtok_r(NULL, "&", &saveptr);
	}
	strip_ext(path);

	if( h->reqflags & FLAG_XFERSTREAMING || h->reqflags & FLAG_RANGE )
	{
		DPRINTF(E_WARN, L_HTTP, "Client tried to specify transferMode as Streaming with a resized image!\n");
		Send406(h);
		goto resized_error;
	}

	sprintf(sql_buf, "SELECT PATH, RESOLUTION, THUMBNAIL from DETAILS where ID = '%s'", path);
	ret = sql_get_table(db, sql_buf, &result, &rows, NULL);
	if( (ret != SQLITE_OK) || !rows || (access(result[3], F_OK) != 0) )
	{
		DPRINTF(E_ERROR, L_HTTP, "Didn't find valid file for %s!\n", path);
		free(path);
		goto resized_error;
	}
	file_path = result[3];
	resolution = result[4];
	tn = result[5];
	DPRINTF(E_INFO, L_HTTP, "Serving resized image for ObjectId: %s [%s]\n", path, file_path);
	free(path);

	/* Resizing from a thumbnail is much faster than from a large image */
	if( width <= 160 && height <= 120 && atoi(tn) )
	{
		l = exif_loader_new();
		exif_loader_write_file(l, file_path);
		ed = exif_loader_get_data(l);
		exif_loader_unref(l);

		if( !ed || !ed->size )
		{
			if( ed )
				exif_data_unref(ed);
			Send404(h);
			sqlite3_free_table(result);
			goto resized_error;
		}
		imsrc = image_new_from_jpeg(NULL, 0, (char *)ed->data, ed->size);
		exif_data_unref(ed);
	}
	else
	{
		imsrc = image_new_from_jpeg(file_path, 1, NULL, 0);
	}
	if( !imsrc )
	{
		Send404(h);
		sqlite3_free_table(result);
		goto resized_error;
	}
	/* Figure out the best destination resolution we can use */
	dstw = width;
	dsth = ((((width<<10)/imsrc->width)*imsrc->height)>>10);
	if( dsth > height )
	{
		dsth = height;
		dstw = (((height<<10)/imsrc->height) * imsrc->width>>10);
	}
	imdst = image_resize(imsrc, dstw, dsth);
	data = image_save_to_jpeg_buf(imdst, &size);

	DPRINTF(E_INFO, L_HTTP, "size: %d\n", size);
	strftime(date, 30,"%a, %d %b %Y %H:%M:%S GMT" , gmtime(&curtime));
	snprintf(header, sizeof(header)-50, "%s 200 OK\r\n"
	                                    "Content-Type: image/jpeg\r\n"
	                                    "Content-Length: %d\r\n"
	                                    "Connection: close\r\n"
	                                    "Date: %s\r\n"
	                                    "EXT:\r\n"
	                                    "contentFeatures.dlna.org: DLNA.ORG_PN=JPEG_TN\r\n"
	                                    "Server: RAIDiator/4.1, UPnP/1.0, MiniDLNA_TN/1.0\r\n",
	                                    h->HttpVer, size, date);

	if( h->reqflags & FLAG_XFERINTERACTIVE )
	{
		strcat(header, "transferMode.dlna.org: Interactive\r\n");
	}
	else if( h->reqflags & FLAG_XFERBACKGROUND )
	{
		strcat(header, "transferMode.dlna.org: Background\r\n");
	}
	strcat(header, "\r\n");

	if( (send_data(h, header, strlen(header)) == 0) && (h->req_command != EHead) )
	{
		send_data(h, (char *)data, size);
	}
	DPRINTF(E_INFO, L_HTTP, "Done serving %s\n", file_path);
	image_free(imsrc);
	image_free(imdst);
	sqlite3_free_table(result);
	resized_error:
#if USE_FORK
	_exit(0);
#endif
}

void
SendResp_dlnafile(struct upnphttp * h, char * object)
{
	char header[1500];
	char hdr_buf[512];
	char sql_buf[256];
	char **result;
	int rows;
	char date[30];
	time_t curtime = time(NULL);
	off_t total, offset, size;
	char *path, *mime, *dlna;
	int sendfh;
#if USE_FORK
	pid_t newpid = 0;
	newpid = fork();
	if( newpid )
		return;
#endif

	memset(header, 0, 1500);

	strip_ext(object);
	sprintf(sql_buf, "SELECT PATH, MIME, DLNA_PN from DETAILS where ID = '%s'", object);
	sqlite3_get_table(db, sql_buf, &result, &rows, 0, 0);
	if( !rows )
	{
		DPRINTF(E_WARN, L_HTTP, "%s not found, responding ERROR 404\n", object);
		Send404(h);
		sqlite3_free_table(result);
		goto done_dlna;
	}
	path = result[3];
	mime = result[4];
	dlna = result[5];

	DPRINTF(E_INFO, L_HTTP, "Serving DetailID: %s [%s]\n", object, path);

	if( h->reqflags & FLAG_XFERSTREAMING )
	{
		if( strncmp(mime, "image", 5) == 0 )
		{
			DPRINTF(E_WARN, L_HTTP, "Client tried to specify transferMode as Streaming with an image!\n");
			Send406(h);
			goto error;
		}
	}
	if( h->reqflags & FLAG_XFERINTERACTIVE )
	{
		if( h->reqflags & FLAG_REALTIMEINFO )
		{
			DPRINTF(E_WARN, L_HTTP, "Bad realTimeInfo flag with Interactive request!\n");
			Send400(h);
			goto error;
		}
		if( strncmp(mime, "image", 5) != 0 )
		{
			DPRINTF(E_WARN, L_HTTP, "Client tried to specify transferMode as Interactive without an image!\n");
			Send406(h);
			goto error;
		}
	}

	strftime(date, 30,"%a, %d %b %Y %H:%M:%S GMT" , gmtime(&curtime));
	offset = h->req_RangeStart;
	sendfh = open(path, O_RDONLY);
	if( sendfh < 0 ) {
		DPRINTF(E_ERROR, L_HTTP, "Error opening %s\n", path);
		goto error;
	}
	size = lseek(sendfh, 0, SEEK_END);
	lseek(sendfh, 0, SEEK_SET);

	sprintf(header, "HTTP/1.1 20%c OK\r\n"
			"Content-Type: %s\r\n", (h->reqflags & FLAG_RANGE ? '6' : '0'), mime);
	if( h->reqflags & FLAG_RANGE )
	{
		if( !h->req_RangeEnd )
			h->req_RangeEnd = size;
		if( (h->req_RangeStart > h->req_RangeEnd) || (h->req_RangeStart < 0) )
		{
			DPRINTF(E_WARN, L_HTTP, "Specified range was invalid!\n");
			Send400(h);
			close(sendfh);
			goto error;
		}
		if( h->req_RangeEnd > size )
		{
			DPRINTF(E_WARN, L_HTTP, "Specified range was outside file boundaries!\n");
			Send416(h);
			close(sendfh);
			goto error;
		}

		if( h->req_RangeEnd < size )
		{
			total = h->req_RangeEnd - h->req_RangeStart + 1;
			sprintf(hdr_buf, "Content-Length: %llu\r\n"
					 "Content-Range: bytes %lld-%lld/%llu\r\n",
					 total, h->req_RangeStart, h->req_RangeEnd, size);
		}
		else
		{
			h->req_RangeEnd = size;
			total = size - h->req_RangeStart;
			sprintf(hdr_buf, "Content-Length: %llu\r\n"
					 "Content-Range: bytes %lld-%llu/%llu\r\n",
					 total, h->req_RangeStart, size-1, size);
		}
	}
	else
	{
		h->req_RangeEnd = size;
		total = size;
		sprintf(hdr_buf, "Content-Length: %llu\r\n", total);
	}
	strcat(header, hdr_buf);

	if( h->reqflags & FLAG_XFERSTREAMING )
	{
		strcat(header, "transferMode.dlna.org: Streaming\r\n");
	}
	else if( h->reqflags & FLAG_XFERBACKGROUND )
	{
		if( strncmp(mime, "image", 5) == 0 )
			strcat(header, "transferMode.dlna.org: Background\r\n");
	}
	else //if( h->reqflags & FLAG_XFERINTERACTIVE )
	{
		if( (strncmp(mime, "video", 5) == 0) ||
		    (strncmp(mime, "audio", 5) == 0) )
		{
			strcat(header, "transferMode.dlna.org: Streaming\r\n");
		}
		else
		{
			strcat(header, "transferMode.dlna.org: Interactive\r\n");
		}
	}

	sprintf(hdr_buf, "Accept-Ranges: bytes\r\n"
			 "Connection: close\r\n"
			 "Date: %s\r\n"
			 "EXT:\r\n"
			 "contentFeatures.dlna.org: DLNA.ORG_PN=%s\r\n"
			 "Server: RAIDiator/4.1, UPnP/1.0, MiniDLNA/1.0\r\n\r\n",
			 date, dlna);
	strcat(header, hdr_buf);

	if( (send_data(h, header, strlen(header)) == 0) && (h->req_command != EHead) && (sendfh > 0) )
	{
		send_file(h, sendfh, offset, h->req_RangeEnd);
	}
	close(sendfh);

	error:
	sqlite3_free_table(result);
	done_dlna:
#if USE_FORK
	_exit(0);
#endif
}
