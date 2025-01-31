
/*
 *   zsync - client side rsync over http
 *   Copyright (C) 2004,2005,2007,2009 Colin Phipps <cph@moria.org.uk>
 *   Copyright (C) 2015 Simon Peter
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the Artistic License v2 (see the accompanying 
 *   file COPYING for the full license terms), or, at your option, any later 
 *   version of the same license.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   COPYING file for details.
 */

/* HTTP client code for zsync.
 * Including pipeline HTTP Range fetching code.  */

#include "zsglobal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netdb.h>
#include <time.h>
#include <curl/curl.h>

#ifndef HAVE_GETADDRINFO
#include "getaddrinfo.h"
#endif

#ifdef WITH_DMALLOC
# include <dmalloc.h>
#endif

#include "http.h"
#include "url.h"
#include "progress.h"
#include "format_string.h"



/****************************************************************************
 *
 * Buffered struct to read from a HTTP connection, managed by curl.
 * Uses the http_* functions to manage it like a file.
 */

struct http_file
{
    union {
        CURL *curl;
        FILE *file;
    } handle;

    char *buffer;
    size_t buffer_len;
    size_t buffer_pos;
    int still_running;
};

typedef struct http_file HTTP_FILE;

/* global curl handle */
CURLM *multi_handle;

int http_ssl_insecure = 0;
int http_verbose = 0;
const char* http_clientauth_key = NULL;
const char* http_clientauth_cert = NULL;
const char* http_cacert = NULL;
const char* http_unix_socket_path = NULL;

char *cookie;

struct range_fetch {
    /* URL to retrieve from, host:port, auth header */
    char *url;
    HTTP_FILE *file;
    char *boundary; /* If we're in the middle of reading a mime/multipart
                     * response, this is the boundary string. */

    /* State for block currently being read */
    size_t block_left;  /* non-zero if we're in the middle of reading a block */
    off_t offset;       /* and this is the offset of the start of the block we are reading */

    /* Keep count of total bytes retrieved */
    off_t bytes_down;

    /* Byte ranges to fetch */
    off_t *ranges_todo; /* Contains 2*nranges ranges, consisting of start and stop offset */
    int nranges;
    int rangessent;     /* We've requested the first rangessent ranges from the remote */
    int rangesdone;     /* and received this many */
};


/****************************************************************************
 *
 * Common set up code for each curl handle we deal with. Sets SSL options,
 * proxy options, etc.
 */
 void setup_curl_handle(CURL *handle)
{
    char *pr = getenv("http_proxy");
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);

    if (pr != NULL){
        curl_easy_setopt(handle, CURLOPT_PROXY, pr);
    }

    if (http_verbose) {
        curl_easy_setopt(handle, CURLOPT_VERBOSE, 1L);
    }

    if(http_ssl_insecure){
        curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 0 );
        curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 0 );
    }

    if(http_clientauth_key){
        curl_easy_setopt(handle, CURLOPT_SSLKEY, http_clientauth_key);
    }

    if(http_clientauth_cert){
        curl_easy_setopt(handle, CURLOPT_SSLCERT, http_clientauth_cert);
    }

    if(http_cacert){
        curl_easy_setopt(handle, CURLOPT_CAINFO, http_cacert);
    }
    
    if(http_unix_socket_path){
        curl_easy_setopt(handle, CURLOPT_UNIX_SOCKET_PATH, http_unix_socket_path);
    }

    if(cookie != NULL){
        curl_easy_setopt(handle, CURLOPT_COOKIE, cookie);
    }

    char* verbose;
    verbose = getenv ("CURLOPT_VERBOSE");
    if (verbose!=NULL){
      curl_easy_setopt(handle, CURLOPT_VERBOSE, 1L);
    }
}

/****************************************************************************
 *
 * Get a file from a url, put the contents into a temporary file.
 * Keeps track of the actual url used after redirects, etc.
 */

FILE *http_get(const char *orig_url, char **track_referer, const char *tfname) {
    FILE *f;
    CURL *curl;
    CURLcode res;
    long response_code;
    char *effective_url;

    f = tfname ? fopen(tfname, "w+") : tmpfile();
    if (!f) {
        perror(tfname);
        return NULL;
    }

    /* this isn't anything to do with the main transfer, so has it's own easy
       curl handle. */
    curl = curl_easy_init();
    if (!curl) {
        fclose(f);
        return NULL;
    }

    /* TODO: set up the SSL options in common code. */
    curl_easy_setopt( curl, CURLOPT_URL, orig_url );
    curl_easy_setopt( curl, CURLOPT_WRITEDATA, f );
    setup_curl_handle(curl);

    res = curl_easy_perform( curl );
    if (res) {
        fprintf(stderr, "libcurl: %s\n", curl_easy_strerror(res));
        fclose(f);
        curl_easy_cleanup(curl);
        return NULL;
    }

    res = curl_easy_getinfo( curl, CURLINFO_RESPONSE_CODE, &response_code );
    if (res != CURLE_OK) {
        fprintf(stderr, "Could not get HTTP response code!\n");
        fclose(f);
        curl_easy_cleanup(curl);
        return NULL;
    }

    if (response_code != 200) {
        fprintf(stderr, "Got HTTP %ld (expected 200)\n", response_code);
        fclose(f);
        curl_easy_cleanup(curl);
        return NULL;
    }

    if (track_referer) {
        res = curl_easy_getinfo( curl, CURLINFO_EFFECTIVE_URL, &effective_url );
        if(res != CURLE_OK) {
            fprintf(stderr, "Could not get last effective URL: %s\n", curl_easy_strerror(res));
            fclose(f);
            curl_easy_cleanup(curl);
            return NULL;
        }

        *track_referer = strdup(effective_url);
    }

    curl_easy_cleanup( curl );
    rewind(f);
    return f;
}


/****************************************************************************
 *
 * curl calls this routine to get more data when doing the main data transfer.
 */
static size_t write_callback(char *buffer, size_t size, size_t nitems, void *userp)
{
    char *newbuff;
    size_t rembuff;
    HTTP_FILE *url = (HTTP_FILE *)userp;
    size *= nitems;
    /* remaining space in buffer */
    rembuff=url->buffer_len - url->buffer_pos;

    if(size > rembuff) {
        /* not enough space in buffer, alloc more */
        newbuff = realloc(url->buffer, url->buffer_len + (size - rembuff));
        if(newbuff == NULL){
            fprintf(stderr,"callback buffer grow failed\n");
            size = rembuff;
        }else{
            /* buffer got more memory, record the new length and keep the new buffer */
            url->buffer_len += size - rembuff;
            url->buffer = newbuff; 
        }
    }

    memcpy(&url->buffer[url->buffer_pos], buffer, size);
    url->buffer_pos += size;
    return size;
}


/****************************************************************************
 *
 * Creates the ranges to fetch. We only send 20 ranges at a time,
 * because with large amounts of differences the string can get too long
 * for the remote server to accept.
 */
void http_load_ranges(struct range_fetch* rf)
{
    /* send max 20 ranges in each request */
    int ranges_limit = 20;
    int sent_this_chunk = 0;
    int l;
    int i;
    /* destination for the final ranges request header content */
    char ranges_opt[4097];
    /* each individual range we prepare */
    char range[32];

    memset(range, 0, sizeof(range));
    memset(ranges_opt, 0, sizeof(ranges_opt));

    /* create ranges with maximum of ranges_limit, or as many as there are */
    for (; (sent_this_chunk < ranges_limit) && (rf->rangessent < rf->nranges); sent_this_chunk++) {
        /* makes the table of ranges access more readable */
        i = rf->rangessent;
        l = strlen(ranges_opt);
        snprintf(range, sizeof(range), OFF_T_PF "-" OFF_T_PF ",",
                 rf->ranges_todo[2 * i], rf->ranges_todo[2 * i + 1]);
        strncat(ranges_opt, range, l + strlen(range));        
        rf->rangessent++;
    }

    /* gets rid of the trailing comma */
    ranges_opt[strlen(ranges_opt)-1] = 0;
    curl_easy_setopt(rf->file->handle.curl, CURLOPT_RANGE, ranges_opt);
}


/****************************************************************************
 *
 * Loads a HTTP_FILE into the range_fetch struct. Can be called multiple
 * times to send requests for further ranges.
 */
HTTP_FILE *http_fetch_ranges(struct range_fetch* rf)
{
    HTTP_FILE *file;
    
    if(!multi_handle){
        multi_handle = curl_multi_init();
    }

    if(!rf->file) {
        /* if the file has never been set, we've never sent any ranges. */
        rf->rangessent = 0;
    }else{
        /* free the old file and buffer, ready for the new one */
        if(rf->file->buffer){
            free(rf->file->buffer);
        }
        free(rf->file);
    }

    file = (HTTP_FILE *) malloc(sizeof(HTTP_FILE));
    memset(file, 0, sizeof(HTTP_FILE));
    file->handle.curl = curl_easy_init();

    /* TODO: move these into common code that sets them based on a command line option */
    setup_curl_handle(file->handle.curl);

    curl_easy_setopt(file->handle.curl, CURLOPT_URL, rf->url);
    curl_easy_setopt(file->handle.curl, CURLOPT_WRITEDATA, file);

    /* we still process the headers ourselves so we can get range information */
    curl_easy_setopt(file->handle.curl, CURLOPT_HEADER, 1L);
    curl_easy_setopt(file->handle.curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_multi_add_handle(multi_handle, file->handle.curl);
    rf->file = file;

    http_load_ranges(rf);
    curl_multi_perform(multi_handle, &rf->file->still_running);

    return rf->file;
}


/****************************************************************************
 *
 * Closes the connection, cleans up curl.
 */
int http_fclose(HTTP_FILE *file)
{
    curl_multi_remove_handle(multi_handle, file->handle.curl);
    curl_easy_cleanup(file->handle.curl);
    if(file->buffer){
        free(file->buffer);
    }
    free(file);
    return 0;
}


/****************************************************************************
 *
 * If there's nothing in the buffer, and no transfer running, we've hit EOF.
 */
int http_feof(HTTP_FILE *file)
{
    if((file->buffer_pos == 0) && (!file->still_running)){
        return 1;
    }
    return 0;
}


static int fill_buffer(HTTP_FILE *file, size_t want)
{
    fd_set fdread;
    fd_set fdwrite;
    fd_set fdexcep;
    struct timeval timeout;
    int result = -1;

    /* only fill buffer if transfers are still running and
       the buffer isn't bigger than the wanted size */
    if((!file->still_running) || (file->buffer_pos > want)){
        return 0;
    }

    /* try to fill the buffer */
    do{
        int maxfd = -1;
        long curl_timeo = -1;

        FD_ZERO(&fdread);
        FD_ZERO(&fdwrite);
        FD_ZERO(&fdexcep);

        /* one minute timeout, could make this an option? */
        timeout.tv_sec = 60;
        timeout.tv_usec = 0;

        curl_multi_timeout(multi_handle, &curl_timeo);
        if(curl_timeo >= 0){
            timeout.tv_sec = curl_timeo / 1000;
            if(timeout.tv_sec > 1){
                timeout.tv_sec = 1;
            }else{
                timeout.tv_usec = (curl_timeo % 1000) * 1000;
            }
        }
        curl_multi_fdset(multi_handle, &fdread, &fdwrite, &fdexcep, &maxfd);
        result = select(maxfd+1, &fdread, &fdwrite, &fdexcep, &timeout);
        switch(result){
            case -1:
                // select error
                break;

            case 0:
            default:
                curl_multi_perform(multi_handle, &file->still_running);
                break;
        }
    } while(file->still_running && (file->buffer_pos < want));
    return 1;
}


/****************************************************************************
 *
 * Removes `want` bytes from the front of the buffer.
 */
static int use_buffer(HTTP_FILE *file, int want)
{
    if((file->buffer_pos - want) <= 0){
        /* trash the buffer */
        if(file->buffer){
            free(file->buffer);
        }
        file->buffer = NULL;
        file->buffer_pos = 0;
        file->buffer_len = 0;
    }else{
        /* move the contents past want down so it's still available */
        memmove(file->buffer, &file->buffer[want], (file->buffer_pos - want));
        file->buffer_pos -= want;
    }
    return 0;
}


/****************************************************************************
 *
 * Reads bytes from a HTTP_FILE.
 */
size_t http_fread(void *ptr, size_t size, size_t nmemb, HTTP_FILE *file)
{
    size_t want;
    want = nmemb * size;
    fill_buffer(file, want);

    if(!file->buffer_pos){
        /* nothing read, nothing in buffer */
        return 0;
    }

    // only consider available data
    if(file->buffer_pos < want){
        want = file->buffer_pos;
    }

    memcpy(ptr, file->buffer, want);
    use_buffer(file, want);

    want = want / size; // number of items
    return want;
}


/* Remember referrer */
char *referer;
char *redirected;
int use_redirected = 0;

/* range_fetch methods */

static int range_fetch_set_url(struct range_fetch* rf, const char* orig_url) {
    free(rf->url);
    rf->url = strdup(orig_url);
    return !!rf->url;
}


char *rfgets(char *ptr, size_t size, struct range_fetch *rf)
{
    size_t want = size - 1;/* always need to leave room for zero termination */ 
    size_t loop;
    HTTP_FILE *file = rf->file;
    fill_buffer(file, want);
 
    /* check if theres data in the buffer - if not fill either errored or
     * EOF */ 

    if(!file->buffer_pos){
      return NULL;
    }

    /* ensure only available data is considered */ 
    if(file->buffer_pos < want)
      want = file->buffer_pos;
 
    /*buffer contains data */ 
    /* look for newline or eof */ 
    for(loop=0;loop < want;loop++) {
      if(file->buffer[loop] == '\n') {
        want=loop+1;/* include newline */ 
        break;
      }
    }
 
    /* xfer data to caller */ 
    memcpy(ptr, file->buffer, want);
    ptr[want]=0;/* allways null terminate */ 
 
    use_buffer(file,want);
  
  return ptr;
}



/* range_fetch_start(origin_url)
 * Returns a new range fetch object, for the given URL.
 */
struct range_fetch *range_fetch_start(const char *orig_url) {
    struct range_fetch *rf = malloc(sizeof(struct range_fetch));
    if (!rf)
        return NULL;

    /* Blank initialisation for other fields before set_url call */
    rf->url = NULL;

    if (!range_fetch_set_url(rf, orig_url)) {
        free(rf);
        return NULL;
    }

    /* Initialise other state fields */
    rf->block_left = 0;
    rf->bytes_down = 0;
    rf->boundary = NULL;
    rf->file = NULL;                        /* http file not open */
    rf->ranges_todo = NULL;             /* And no ranges given yet */
    rf->nranges = rf->rangesdone = 0;

    return rf;
}




/* range_fetch_addranges(self, off_t[], nranges)
 * Adds ranges to fetch, supplied as an array of 2*nranges offsets (start and
 * stop for each range) */
void range_fetch_addranges(struct range_fetch *rf, off_t * ranges, int nranges) {
    int existing_ranges = rf->nranges - rf->rangesdone;

    /* Allocate new memory, enough for valid existing entries and new entries */
    off_t *nr = malloc(2 * sizeof(*ranges) * (nranges + existing_ranges));
    if (!nr)
        return;

    /* Copy only still-valid entries from the existing queue over */
    memcpy(nr, &(rf->ranges_todo[2 * rf->rangesdone]),
           2 * sizeof(*ranges) * existing_ranges);

    /* And replace existing queue with new one */
    free(rf->ranges_todo);
    rf->ranges_todo = nr;
    rf->rangessent -= rf->rangesdone;
    rf->rangesdone = 0;
    rf->nranges = existing_ranges;

    /* And append the new stuff */
    memcpy(&nr[2 * existing_ranges], ranges, 2 * sizeof(*ranges) * nranges);
    rf->nranges += nranges;
}


/* buflwr(str) - in-place convert this string to lower case */
static void buflwr(char *s) {
    char c;
    while ((c = *s) != 0) {
        if (c >= 'A' && c <= 'Z')
            *s = c - 'A' + 'a';
        s++;
    }
}

/* range_fetch_read_http_headers - read a set of HTTP headers, updating state
 * appropriately.
 * Returns: EOF returns 0, good returns 206 (reading a range block) or 30x
 *  (redirect), error returns <0 */
int range_fetch_read_http_headers(struct range_fetch *rf) {
    char buf[512];
    int status;
    int seen_location = 0;

    {                           /* read status line */
        char *p;
        
        if (rfgets(buf, sizeof(buf), rf) == NULL){
            /* most likely unexpected EOF from server */
            return -1;
        }
        if (buf[0] == 0)
            return 0;           /* EOF, caller decides if that's an error */
        if (memcmp(buf, "HTTP/1", 6) != 0 || (p = strchr(buf, ' ')) == NULL) {
            fprintf(stderr, "got non-HTTP response '%s'\n", buf);
            return -1;
        }
        status = atoi(p + 1);
        if (status != 206 && status != 301 && status != 302) {
            if (status >= 300 && status < 400) {
                fprintf(stderr,
                        "\nzsync received a redirect/further action required status code: %d\nzsync specifically refuses to proceed when a server requests further action. This is because zsync makes a very large number of requests per file retrieved, and so if zsync has to perform additional actions per request, it further increases the load on the target server. The person/entity who created this zsync file should change it to point directly to a URL where the target file can be retrieved without additional actions/redirects needing to be followed.\nSee http://zsync.moria.orc.uk/server-issues\n",
                        status);
            }
            else if (status == 200) {
                fprintf(stderr,
                        "\nzsync received a data response (code %d) but this is not a partial content response\nzsync can only work with servers that support returning partial content from files. The person/entity creating this .zsync has tried to use a server that is not returning partial content. zsync cannot be used with this server.\nSee http://zsync.moria.orc.uk/server-issues\n",
                        status);
            }
            else {
                /* generic error message otherwise */
                fprintf(stderr, "bad status code %d\n", status);
            }
            return -1;
        }
    }

    /* Read other headers */
    while (1) {
        char *p;

        /* Get next line */
        if (rfgets(buf, sizeof(buf), rf) == NULL)
            return -1;

        /* If it's the end of the headers */
        if (buf[0] == '\r' || buf[0] == '\0') {
            /* We are happy provided we got the block boundary, or an actual block is starting. */
            if (((rf->boundary || rf->block_left)
                 && !(rf->boundary && rf->block_left))
                || (status >= 300 && status < 400 && seen_location))
                return status;
            break;
        }

        /* Parse header */
        p = strstr(buf, ": ");
        if (!p)
            break;
        *p = 0;
        p += 2;
        buflwr(buf);
        {   /* Remove the trailing \r\n from the value */
            int len = strcspn(p, "\r\n");
            p[len] = 0;
        }
        /* buf is the header name (lower-cased), p the value */
        /* Switch based on header */

        if (status == 206 && !strcmp(buf, "content-range")) {
            /* Okay, we're getting a non-MIME block from the remote. Get the
             * range and set our state appropriately */
            off_t from, to;
            sscanf(p, "bytes " OFF_T_PF "-" OFF_T_PF "/", &from, &to);
            if (from <= to) {
                rf->block_left = to + 1 - from;
                rf->offset = from;
            }

            /* Can only have got one range. */
            rf->rangesdone++;
            rf->rangessent = rf->rangesdone;
        }

        /* If we're about to get a MIME multipart block set */
        if (status == 206 && !strcasecmp(buf, "content-type")
            && !strncasecmp(p, "multipart/byteranges", 20)) {

            /* Get the multipart boundary string */
            char *q = strstr(p, "boundary=");
            if (!q)
                break;
            q += 9;

            /* Gah, we could really use a regexp here. Could be quoted... */
            if (*q == '"') {
                rf->boundary = strdup(q + 1);
                q = strchr(rf->boundary, '"');
                if (q)
                    *q = 0;
            }
            else {  /* or unquoted */
                rf->boundary = strdup(q);
                q = rf->boundary + strlen(rf->boundary) - 1;

                while (*q == '\r' || *q == ' ' || *q == '\n')
                    *q-- = '\0';
            }
        }

        /* No other headers that we care about. In particular:
         *
         * FIXME: non-conformant to HTTP/1.1 because we ignore
         * Transfer-Encoding: chunked.
         */
    }
    return -1;
}

/* get_range_block(self, &offset, buf[], buflen)
 *
 * This is where it all happens. This is a complex function to present a very
 * simple read(2)-like interface to the caller over the top of all the HTTP
 * going on.
 *
 * It returns blocks of actual data, retrieved from the origin URL, to the
 * caller. Data is returned in the buffer, up to the specified length, and the
 * offset in the file from which the data comes is written to the offset
 * parameter.
 *
 * Like read(2), it returns the total bytes read, 0 for EOF, -1 for error.
 *
 * The blocks that it returns are the ones previously registered by calls to
 * range_fetch_addranges (although it doesn't guarantee that only those block
 * are returned - that's just what it asks the remote for, but if the remote
 * returns more then it'll pass more to the caller - which doesn't matter).
 */
int get_range_block(struct range_fetch *rf, off_t * offset, unsigned char *data,
                    size_t dlen) {
    size_t bytes_to_caller = 0;
    size_t bytes_to_request = 0;

    /* If we're not in the middle of reading a block of actual data */
    if (!rf->block_left) {

      check_boundary:
        /* And if not reading a MIME multipart boundary */
        if (!rf->boundary) {
            if (rf->rangesdone == rf->nranges){
                // nothing left to do, all requests finished, return and end :)
                return 0;
            }
            /* Then we're reading the start of a new set of HTTP headers
             * (possibly after connecting and sending a request first. */
            int header_result;
            http_fetch_ranges(rf);

            /* read the response headers */
            header_result = range_fetch_read_http_headers(rf);
            //fprintf(stdout, "header_result of %d\n\n", header_result);

            /* EOF on first connect is fatal */
            if (header_result == 0) {
                fprintf(stderr, "EOF from %s\n", rf->url);
                return -1;
            }

            /* Return EOF or error to caller */
            if (header_result <= 0){
                fprintf(stderr, "Other error? %d\n", header_result);
                return header_result ? -1 : 0;
            }
        }

        /* Okay, if we're (now) reading a MIME boundary */
        if (rf->boundary) {
            /* Throw away blank line */
            char buf[512];
            int gotr = 0;
            if (!rfgets(buf, sizeof(buf), rf))
                return 0;

            /* Get, hopefully, boundary marker line */
            if (!rfgets(buf, sizeof(buf), rf))
                return 0;
            if (buf[0] != '-' || buf[1] != '-')
                return 0;

            if (memcmp(&buf[2], rf->boundary, strlen(rf->boundary))) {
                fprintf(stderr, "got bad block boundary: %s != %s",
                        rf->boundary, buf);
                return -1;      /* This is an error now */
            }

            /* Last record marker has boundary followed by - */
            if (buf[2 + strlen(rf->boundary)] == '-') {
                free(rf->boundary);
                rf->boundary = NULL;
                goto check_boundary;
            }

            /* Otherwise, we're reading the MIME headers for this part until we get \r\n alone */
            for (; buf[0] != '\r' && buf[0] != '\n' && buf[0] != '\0';) {
                off_t from, to;

                /* Get next header */
                if (!rfgets(buf, sizeof(buf), rf))
                    return 0;
                buflwr(buf);  /* HTTP headers are case insensitive */

                /* We're looking for the Content-Range: header, to tell us how
                 * many bytes and what part of the target file they represent.
                 */
                if (2 ==
                    sscanf(buf,
                           "content-range: bytes " OFF_T_PF "-" OFF_T_PF "/",
                           &from, &to)) {
                    rf->offset = from;
                    rf->block_left = to - from + 1;
                    gotr = 1;
                }
            }

            /* If we didn't get the byte range that this block represents, it's busted. */
            if (!gotr) {
                fprintf(stderr,
                        "got multipart/byteranges but no Content-Range?");
                return -1;
            }

            /* Else, record that this range is (being) received */
            rf->rangesdone++;
        }
    }

    /* Now the easy bit - we are reading a block of actual data */
    if (!rf->block_left)
        return 0;   /* pass EOF back to caller */
    *offset = rf->offset;   /* caller wants to know what this data is */

    /* don't request more than we want or have */
    if(dlen > rf->block_left){
        bytes_to_request = rf->block_left;
    }else{
        bytes_to_request = dlen;
    }

    bytes_to_caller = http_fread(data, 1, bytes_to_request, rf->file);
    
    /* update internal stats of how many blocks are left,
       file offset, and bytes downloaded. */
    rf->block_left -= bytes_to_caller;
    rf->offset += bytes_to_caller;
    rf->bytes_down += bytes_to_caller;
    
    return bytes_to_caller;
}

/* range_fetch_bytes_down
 * Simple getter method, returns the total bytes retrieved */
off_t range_fetch_bytes_down(const struct range_fetch * rf) {
    return rf->bytes_down;
}

/* Destructor */
void range_fetch_end(struct range_fetch *rf) {
    /* this will clean up the file, buffer, and close the connection */
    if (rf->file != NULL)
        http_fclose(rf->file);

    free(rf->ranges_todo);
    free(rf->boundary);
    free(rf->url);
    free(rf);
}
