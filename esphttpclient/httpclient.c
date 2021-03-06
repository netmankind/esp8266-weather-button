/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * Martin d'Allens <martin.dallens@gmail.com> wrote this file. As long as you retain
 * this notice you can do whatever you want with this stuff. If we meet some day,
 * and you think this stuff is worth it, you can buy me a beer in return.
 * ----------------------------------------------------------------------------
 */

// FIXME: sprintf->snprintf everywhere.

#include "osapi.h"
#include "user_interface.h"
#include "mem.h"
#include "limits.h"
#include "lwip/tcp.h"
#include "lwip/dns.h"
#include "httpclient.h"

// Debug output.
#if 1
#define PRINTF(...) os_printf(__VA_ARGS__)
#else
#define PRINTF(...)
#endif

typedef enum {
	PS_PARSING_HEADER,
	PS_PARSING_BODY,
	PS_PARSING_CHUNKED_BODY
} header_parse_state;

// Internal state.
typedef struct {
	char * path;
	int port;
	char * post_data;
	char * headers;
	char * hostname;
	char * buffer;
	int buffer_size;
	bool secure;
	http_callback user_callback;
	int current_chunk_size;
	int buffer_allocated_size;
	header_parse_state parse_state;
} request_args;

int DEBUG_printf(const char *fmt, ...)
{
	/* Dummy DEBUG_printf required by the open source lwIP implementation */
	return 0;
}

static char * ICACHE_FLASH_ATTR esp_strdup(const char * str)
{
	if (str == NULL) {
		return NULL;
	}
	char * new_str = (char *)os_malloc(os_strlen(str) + 1); // 1 for null character
	if (new_str == NULL) {
		os_printf("esp_strdup: malloc error");
		return NULL;
	}
	os_strcpy(new_str, str);
	return new_str;
}

static int ICACHE_FLASH_ATTR
esp_isupper(char c)
{
    return (c >= 'A' && c <= 'Z');
}

static int ICACHE_FLASH_ATTR
esp_isalpha(char c)
{
    return ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'));
}


static int ICACHE_FLASH_ATTR
esp_isspace(char c)
{
    return (c == ' ' || c == '\t' || c == '\n' || c == '\12');
}

static int ICACHE_FLASH_ATTR
esp_isdigit(char c)
{
    return (c >= '0' && c <= '9');
}

/*
 * Convert a string to a long integer.
 *
 * Ignores `locale' stuff.  Assumes that the upper and lower case
 * alphabets and digits are each contiguous.
 */
static long ICACHE_FLASH_ATTR
esp_strtol(const char *nptr, char **endptr, int base)
{
	const char *s = nptr;
	unsigned long acc;
	int c;
	unsigned long cutoff;
	int neg = 0, any, cutlim;

	/*
	 * Skip white space and pick up leading +/- sign if any.
	 * If base is 0, allow 0x for hex and 0 for octal, else
	 * assume decimal; if base is already 16, allow 0x.
	 */
	do {
		c = *s++;
	} while (esp_isspace(c));
	if (c == '-') {
		neg = 1;
		c = *s++;
	} else if (c == '+')
		c = *s++;
	if ((base == 0 || base == 16) &&
	    c == '0' && (*s == 'x' || *s == 'X')) {
		c = s[1];
		s += 2;
		base = 16;
	} else if ((base == 0 || base == 2) &&
	    c == '0' && (*s == 'b' || *s == 'B')) {
		c = s[1];
		s += 2;
		base = 2;
	}
	if (base == 0)
		base = c == '0' ? 8 : 10;

	/*
	 * Compute the cutoff value between legal numbers and illegal
	 * numbers.  That is the largest legal value, divided by the
	 * base.  An input number that is greater than this value, if
	 * followed by a legal input character, is too big.  One that
	 * is equal to this value may be valid or not; the limit
	 * between valid and invalid numbers is then based on the last
	 * digit.  For instance, if the range for longs is
	 * [-2147483648..2147483647] and the input base is 10,
	 * cutoff will be set to 214748364 and cutlim to either
	 * 7 (neg==0) or 8 (neg==1), meaning that if we have accumulated
	 * a value > 214748364, or equal but the next digit is > 7 (or 8),
	 * the number is too big, and we will return a range error.
	 *
	 * Set any if any `digits' consumed; make it negative to indicate
	 * overflow.
	 */
	cutoff = neg ? -(unsigned long)LONG_MIN : LONG_MAX;
	cutlim = cutoff % (unsigned long)base;
	cutoff /= (unsigned long)base;
	for (acc = 0, any = 0;; c = *s++) {
		if (esp_isdigit(c))
			c -= '0';
		else if (esp_isalpha(c))
			c -= esp_isupper(c) ? 'A' - 10 : 'a' - 10;
		else
			break;
		if (c >= base)
			break;
		if (any < 0 || acc > cutoff || (acc == cutoff && c > cutlim))
			any = -1;
		else {
			any = 1;
			acc *= base;
			acc += c;
		}
	}
	if (any < 0) {
		acc = neg ? LONG_MIN : LONG_MAX;
//		errno = ERANGE;
	} else if (neg)
		acc = -acc;
	if (endptr != 0)
		*endptr = (char *)(any ? s - 1 : nptr);
	return (acc);
}

static void ICACHE_FLASH_ATTR do_cleanup(void * arg) {
	if (arg != NULL) {
		request_args * req = (request_args *)arg;
		os_free(req->buffer);
		os_free(req->hostname);
		os_free(req->path);
		os_free(req);
	}
}

static void ICACHE_FLASH_ATTR disconnect_callback(void * arg)
{
	PRINTF("Disconnected\n");

	if(arg != NULL) {
		request_args * req = (request_args *)arg;
		if (req->user_callback != NULL) {
			req->user_callback(NULL, HTTP_STATUS_DISCONNECT, NULL, 0);
		}
	}

	do_cleanup(arg);
}

static bool ICACHE_FLASH_ATTR append_to_buffer(request_args * req,
	char * data, unsigned short len) {
	const int new_size = req->buffer_size + len;

	// Let's do the equivalent of a realloc().
	if (new_size > req->buffer_allocated_size) {
		char * new_buffer;
		if (new_size > BUFFER_SIZE_MAX ||
			NULL == (new_buffer = (char *)os_malloc(new_size))) {
			os_printf("Response too long (%d)\n", new_size);
			req->buffer[0] = '\0'; // Discard the buffer to avoid using an incomplete response.
			return false;
		}

		os_memcpy(new_buffer, req->buffer, req->buffer_size);
		os_free(req->buffer);
		req->buffer = new_buffer;
		req->buffer_allocated_size = new_size;
	}

	os_memcpy(req->buffer + req->buffer_size - 1 /*overwrite the null character*/, data, len); // Append new data.
	req->buffer[new_size - 1] = '\0'; // Make sure there is an end of string.
	req->buffer_size = new_size;

	return true;
}

static bool ICACHE_FLASH_ATTR parse_header(const char * buffer,
	int * http_status, bool * is_chunked, char ** body_start) {
	// FIXME: make sure this is not a partial response, using the Content-Length header.
	const char * version10 = "HTTP/1.0 ";
	const char * version11 = "HTTP/1.1 ";
	if (os_strncmp(buffer, version10, strlen(version10)) != 0
	 && os_strncmp(buffer, version11, strlen(version11)) != 0) {
		os_printf("Invalid version in %s\n", buffer);
		return false;
	}

	if (http_status != NULL) {
		*http_status = atoi(buffer + strlen(version10));
	}
	/* find the start of the body */
	if (body_start != NULL) {
		char * header_end = (char *)os_strstr(buffer, "\r\n\r\n");
		if (header_end != NULL) {
			*body_start = header_end + 2;
		} else {
			*body_start = NULL;
		}
	}

	if (is_chunked != NULL) {
		*is_chunked = os_strstr(buffer, "Transfer-Encoding: chunked") != NULL;
	}
	return true;
}

static err_t ICACHE_FLASH_ATTR receive_callback(void * arg,
	struct tcp_pcb * pcb, struct pbuf * p, err_t err) {
	request_args * req = (request_args *)arg;

	if (p == NULL) {
		disconnect_callback(arg);
		return ERR_OK;
	}

	char * buf = p->payload;
	size_t len = p->len;

	if (req->buffer == NULL) {
		goto success;
	}

	if (!append_to_buffer(req, buf, len)) {
		tcp_close(pcb);
		return ERR_MEM;
	}

	if (req->parse_state == PS_PARSING_HEADER) {
		char *header_end = os_strstr(req->buffer, "\r\n\r\n") + 2;
		if (header_end == NULL) {
			// Not ready to parse the header yet
			PRINTF("partial header\n");
			goto success;
		}
		int http_status = 0;
		bool is_chunked = false;
		if (!parse_header(req->buffer, &http_status, &is_chunked, NULL)) {
			tcp_recved(pcb, len);
			tcp_close(pcb);
			return ERR_MEM;
		} else {
			if (is_chunked) {
				req->parse_state = PS_PARSING_CHUNKED_BODY;
			} else {
				req->parse_state = PS_PARSING_BODY;
			}

			*header_end = '\0';
			if (req->user_callback != NULL) {
				req->user_callback(NULL, http_status, req->buffer, 0);
			}
			*header_end = '\r';

			/* In the case of a chunked body, leave the last CRLF in.
			 * This way the parsing loop can just look for the
			 * \r\nCHUNK SIZE\r\n boundary every time. */
			char *body_start = req->parse_state == PS_PARSING_CHUNKED_BODY ?
				header_end : header_end + 2;
			/* Move the start of the body to the beginning of the buffer */
			int body_offset = body_start - req->buffer;
			if (body_offset < req->buffer_size) {
				req->buffer_size -= body_offset;
				os_memmove(req->buffer, body_start, req->buffer_size);
			} else {
				req->buffer_size = 1;
				req->buffer[0] = '\0';
			}
		}
	}

	if (req->user_callback == NULL) {
		goto success;
	}

	if (req->parse_state == PS_PARSING_CHUNKED_BODY) {
		char *read_ptr = req->buffer;
		char *buffer_end = req->buffer + req->buffer_size - 1;  /* Ignore trailing zero */
		do {
			if (req->current_chunk_size == 0) {
				char *crlf_pos_1 = os_strstr(read_ptr, "\r\n");
				char *crlf_pos_2 = NULL;
				if (crlf_pos_1 != NULL &&
					(crlf_pos_2 = os_strstr(crlf_pos_1 + 2, "\r\n"))) {
					req->current_chunk_size =
						esp_strtol(crlf_pos_1 + 2, (char **)NULL, 16);
					read_ptr = crlf_pos_2 + 2;
				} else { break; }
			}

			char *read_end = read_ptr + req->current_chunk_size;
			if (read_end > buffer_end) {
				read_end = buffer_end;
			}
			size_t read_size = read_end - read_ptr;
			if (read_size > 0) {
				/* Null terminate the current block for convenience */
				char tmp = *read_end;
				*read_end = '\0';
				req->user_callback(read_ptr, HTTP_STATUS_BODY, NULL,
					read_size + 1);
				*read_end = tmp;
			}
			req->current_chunk_size -= read_size;
			read_ptr += read_size;
		} while (read_ptr < buffer_end);

		int leftover_size = buffer_end - read_ptr;
		if (leftover_size < 0) {
			leftover_size = 0;
		}
		/* The trailing zero gets copied here */
		os_memmove(req->buffer, read_ptr, leftover_size + 1);
		req->buffer_size = leftover_size + 1;
	} else if (req->parse_state == PS_PARSING_BODY) {
		req->user_callback(req->buffer, HTTP_STATUS_BODY, NULL,
			req->buffer_size);
		req->buffer[0] = '\0';
		req->buffer_size = 1;
	}

success:
	tcp_recved(pcb, len);
	pbuf_free(p);
	return ERR_OK;
}

static err_t ICACHE_FLASH_ATTR sent_callback(void * arg, struct tcp_pcb * pcb,
	uint16_t len)
{
	request_args * req = (request_args *)arg;

	if (req->post_data == NULL) {
		PRINTF("All sent\n");
		tcp_sent(pcb, NULL);
	}
	else {
		// The headers were sent, now send the contents.
		PRINTF("Sending request body\n");
		tcp_write(pcb, (void *)req->post_data, strlen(req->post_data),
			TCP_WRITE_FLAG_COPY);
		tcp_output(pcb);
		os_free(req->post_data);
		req->post_data = NULL;
	}

	return ERR_OK;
}

static err_t ICACHE_FLASH_ATTR connect_callback(void * arg,
	struct tcp_pcb * pcb, err_t err)
{
	PRINTF("Connected\n");
	request_args * req = (request_args *)arg;

	tcp_sent(pcb, sent_callback);
	tcp_recv(pcb, receive_callback);

	const char * method = "GET";
	char post_headers[32] = "";

	if (req->post_data != NULL) { // If there is data this is a POST request.
		method = "POST";
		os_sprintf(post_headers, "Content-Length: %d\r\n", strlen(req->post_data));
	}

	char buf[69 + strlen(method) + strlen(req->path) + strlen(req->hostname) +
			 strlen(req->headers) + strlen(post_headers)];
	int len = os_sprintf(buf,
						 "%s %s HTTP/1.1\r\n"
						 "Host: %s:%d\r\n"
						 "Connection: close\r\n"
						 "User-Agent: ESP8266\r\n"
						 "%s"
						 "%s"
						 "\r\n",
						 method, req->path, req->hostname, req->port, req->headers, post_headers);

	tcp_write(pcb, (void *)buf, len, TCP_WRITE_FLAG_COPY);
	os_free(req->headers);
	req->headers = NULL;
	PRINTF("Sending request header\n");
	return ERR_OK;
}

static void ICACHE_FLASH_ATTR error_callback(void *arg, sint8 errType)
{
	PRINTF("Disconnected with error\n");
	disconnect_callback(arg);
}

static void ICACHE_FLASH_ATTR dns_callback(const char * hostname, ip_addr_t * addr, void * arg)
{
	request_args * req = (request_args *)arg;

	if (addr == NULL) {
		os_printf("DNS failed for %s\n", hostname);
		if (req->user_callback != NULL) {
			req->user_callback("", -1, "", 0);
		}
		os_free(req->buffer);
		os_free(req->post_data);
		os_free(req->headers);
		os_free(req->path);
		os_free(req->hostname);
		os_free(req);
	}
	else {
		PRINTF("DNS found %s " IPSTR "\n", hostname, IP2STR(addr));

		struct tcp_pcb * pcb = tcp_new();
		if (pcb == NULL) {
			os_printf("Error allocating pcb\n");
		}
		tcp_arg(pcb, (void *)req);

		tcp_err(pcb, error_callback);
		tcp_connect(pcb, addr, req->port, connect_callback);
	}
}

static void ICACHE_FLASH_ATTR do_http_raw_request(const char * hostname, int port, bool secure, const char * path, const char * post_data, const char * headers, http_callback user_callback)
{
	PRINTF("DNS request\n");

	request_args * req = (request_args *)os_malloc(sizeof(request_args));
	req->hostname = esp_strdup(hostname);
	req->path = esp_strdup(path);
	req->port = port;
	req->secure = secure;
	req->headers = esp_strdup(headers);
	req->post_data = esp_strdup(post_data);
	req->buffer_size = 1;
	req->buffer_allocated_size = 1;
	req->buffer = (char *)os_malloc(1);
	req->buffer[0] = '\0'; // Empty string.
	req->user_callback = user_callback;
	req->parse_state = PS_PARSING_HEADER;
	req->current_chunk_size = 0;

	struct ip_addr addr;
	err_t error = dns_gethostbyname(hostname, &addr, dns_callback, req);

	if (error == ERR_INPROGRESS) {
		PRINTF("DNS pending\n");
	}
	else if (error == ERR_OK) {
		// Already in the local names table (or hostname was an IP address), execute the callback ourselves.
		dns_callback(hostname, &addr, req);
	}
}

void ICACHE_FLASH_ATTR http_raw_request(const char * hostname, int port, bool secure, const char * path, const char * post_data, const char * headers, http_callback user_callback) {
	do_http_raw_request(hostname, port, secure, path, post_data, headers, user_callback);
}

static void ICACHE_FLASH_ATTR do_http_post(const char * url, const char * post_data, const char * headers, http_callback user_callback)
{
	// FIXME: handle HTTP auth with http://user:pass@host/
	// FIXME: get rid of the #anchor part if present.

	char hostname[128] = "";
	int port = 80;
	bool secure = false;

	bool is_http  = os_strncmp(url, "http://",  strlen("http://"))  == 0;
	bool is_https = os_strncmp(url, "https://", strlen("https://")) == 0;

	if (is_http)
		url += strlen("http://"); // Get rid of the protocol.
	else if (is_https) {
		port = 443;
		secure = true;
		url += strlen("https://"); // Get rid of the protocol.
	} else {
		os_printf("URL is not HTTP or HTTPS %s\n", url);
		return;
	}

	char * path = os_strchr(url, '/');
	if (path == NULL) {
		path = os_strchr(url, '\0'); // Pointer to end of string.
	}

	char * colon = os_strchr(url, ':');
	if (colon > path) {
		colon = NULL; // Limit the search to characters before the path.
	}

	if (colon == NULL) { // The port is not present.
		os_memcpy(hostname, url, path - url);
		hostname[path - url] = '\0';
	}
	else {
		port = atoi(colon + 1);
		if (port == 0) {
			os_printf("Port error %s\n", url);
			return;
		}

		os_memcpy(hostname, url, colon - url);
		hostname[colon - url] = '\0';
	}


	if (path[0] == '\0') { // Empty path is not allowed.
		path = "/";
	}

	PRINTF("hostname=%s\n", hostname);
	PRINTF("port=%d\n", port);
	PRINTF("path=%s\n", path);
	do_http_raw_request(hostname, port, secure, path, post_data, headers, user_callback);
}

/*
 * Parse an URL of the form http://host:port/path
 * <host> can be a hostname or an IP address
 * <port> is optional
 */
void ICACHE_FLASH_ATTR http_post(const char * url, const char * post_data, const char * headers, http_callback user_callback) {
	do_http_post(url, post_data, headers, user_callback);
}

void ICACHE_FLASH_ATTR http_get(const char * url, const char * headers, http_callback user_callback)
{
	do_http_post(url, NULL, headers, user_callback);
}

void ICACHE_FLASH_ATTR http_callback_example(char * response_body, int http_status, char * response_headers, int body_size)
{
	if (http_status > 0 && response_headers != NULL) {
		os_printf("Received HTTP response with status %d and headers:\n%s\n",
			http_status, response_headers);
	} else if (http_status == HTTP_STATUS_BODY) {
		os_printf("Received a part of the response body:\n%s\n",
			response_body);
	} else if (http_status == HTTP_STATUS_DISCONNECT) {
		os_printf("The response has ended.\n");
	}
}

