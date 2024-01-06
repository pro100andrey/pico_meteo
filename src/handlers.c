#include "pico/cyw43_arch.h"

/*
 * Include picow_http/http.h for picow-http's public API.
 * cmake configuration ensures that it is on the include path.
 */
#include "picow_http/http.h"

#include "handlers.h"
#include "utils.h"

// Custom handler for GET/HEAD /get_sensor_data
err_t sensor_handler(struct http *http, void *p)
{
	(void)p;

	struct resp *resp = http_resp(http);
	err_t err;

	// Get the current temperature value.
	sensor_data_t data = get_sensor_data();
	char *body = new_string("{\"temperature\":%.2f,\"humidity\":%.2f,\"pressure\":%.2f}",
							data.temperature,
							data.humidity,
							data.pressure);

	size_t body_len = strlen(body);
	// Set the Content-Length response header.
	if ((err = http_resp_set_len(resp, body_len)) != ERR_OK)
	{
		free(body);
		HTTP_LOG_ERROR("http_resp_set_len() failed: %d", err);

		return http_resp_err(http, HTTP_STATUS_INTERNAL_SERVER_ERROR);
	}
	// Set the Content-Type response header, in this case to "application/json".
	if ((err = http_resp_set_type_ltrl(resp, "application/json")) != ERR_OK)
	{
		free(body);
		HTTP_LOG_ERROR("http_resp_set_type_ltrl() failed: %d", err);

		return http_resp_err(http, HTTP_STATUS_INTERNAL_SERVER_ERROR);
	}
	// Set the Cache-Control header to "no-store", to indicate that the response
	// is not cacheable.
	if ((err = http_resp_set_hdr_ltrl(resp, "Cache-Control", "no-store")) != ERR_OK)
	{
		free(body);
		HTTP_LOG_ERROR("Set header Cache-Control failed: %d", err);

		return http_resp_err(http, HTTP_STATUS_INTERNAL_SERVER_ERROR);
	}

	err = http_resp_send_buf(http, body, body_len, false);
	free(body);

	return err;
}

/* These will be used for JSON boolean values. */
static const char *bool_str[] = {"false", "true"};

/*
 * Crude implementation of JSON using hard-wired strings and printf
 * formatting.
 *
 * The sample application is built so that it doesn't include any
 * additional libraries besides picow-http and the SDK. But consider using
 * a proper library for more sophisticated implementations of JSON.
 */
#define RSSI_FMT ("{\"valid\":%s,\"rssi\":%d}")
#define RSSI_MAX ("{\"valid\":false,\"rssi\":-2147483648}")
#define RSSI_MAX_LEN (STRLEN_LTRL(RSSI_MAX))

/*
 * Custom handler for GET/HEAD /rssi
 */
err_t rssi_handler(struct http *http, void *p)
{
	/* As above, get the resp object from http_resp(). */
	struct resp *resp = http_resp(http);
	err_t err;
	int32_t rssi;
	int idx;
	char body[RSSI_MAX_LEN];
	size_t body_len;
	(void)p;

	/*
	 * Get the most recent rssi value. get_rssi() returns INT16_MAX if
	 * the value is invalid.
	 */
	rssi = get_rssi();

	/* bool_str[idx] will be the value of the "valid" field. */
	idx = bool_to_bit(rssi != INT32_MAX);

	/*
	 * Format the response body. The return value of snprintf() will
	 * be the size of the body.
	 */
	body_len = snprintf(body, RSSI_MAX_LEN, RSSI_FMT, bool_str[idx], rssi);

	/*
	 * As with the previous handlers, use http_resp_set_len() to set
	 * the Content-Length response header.
	 */
	if ((err = http_resp_set_len(resp, body_len)) != ERR_OK)
	{
		HTTP_LOG_ERROR("http_resp_set_len() failed: %d", err);
		return http_resp_err(http, HTTP_STATUS_INTERNAL_SERVER_ERROR);
	}

	/*
	 * As with the previous handlers, use http_resp_set_type_ltrl() to
	 * set the Content-Type response header, in this case to
	 * "application/json".
	 */
	if ((err = http_resp_set_type_ltrl(resp, "application/json")) != ERR_OK)
	{
		HTTP_LOG_ERROR("http_resp_set_type_ltrl() failed: %d", err);
		return http_resp_err(http, HTTP_STATUS_INTERNAL_SERVER_ERROR);
	}

	/*
	 * As with the previous handlers, use http_resp_set_hdr_ltrl() to
	 * set the Cache-Control header to "no-store", to indicate that
	 * the response is not cacheable.
	 */
	if ((err = http_resp_set_hdr_ltrl(resp, "Cache-Control", "no-store")) != ERR_OK)
	{
		HTTP_LOG_ERROR("Set header Cache-Control failed: %d", err);
		return http_resp_err(http, HTTP_STATUS_INTERNAL_SERVER_ERROR);
	}

	/*
	 * As with the previous handlers, use http_resp_send_buf() to send
	 * the response body. Set the 'durable' parameter to false, since
	 * the body is in a local array.
	 */

	return http_resp_send_buf(http, body, body_len, false);
}

/*
 * Even cruder JSON handling
 */
#define INFO_FMT                                                     \
	("{\"ssid\":\"" WIFI_SSID "\",\"host\":\"" CYW43_HOST_NAME "\"," \
	 "\"ip\":\"%s\",\"mac\":\"%s\"}")
#define INFO_STR                                                     \
	("{\"ssid\":\"" WIFI_SSID "\",\"host\":\"" CYW43_HOST_NAME "\"," \
	 "\"ip\":\"\",\"mac\":\"\"}")
#define INFO_MAX_LEN (STRLEN_LTRL(INFO_STR) + IPADDR_STRLEN_MAX + MAC_ADDR_LEN)

/* The next handler will set an ETag header with a 32-bit value in hex. */
#define ETAG_LEN (sizeof("\"12345678\""))

/* Simple string hash used to generate an ETag header. */
static inline uint32_t
update_hash(const char *p, uint32_t h)
{
	for (; *p != '\0'; p++)
		h = 31 * h + *p;
	return h;
}

/*
 * The ETag header for /netinfo is formed from a hash of the four strings
 * that make up its content: IP address, MAC address, AP SSID, and
 * hostname.
 */
static void
set_etag(char etag[], netinfo_t *info)
{
	uint32_t hash = 0;

	hash = update_hash(info->ip, hash);
	hash = update_hash(info->mac, hash);
	hash = update_hash(WIFI_SSID, hash);
	hash = update_hash(CYW43_HOST_NAME, hash);
	snprintf(etag, ETAG_LEN, "\"%08x\"", hash);
}

/*
 * Custom handler for GET/HEAD /netinfo
 *
 * The response body is a JSON with fields for the SSID of the access
 * point, the PicoW's hostname, its IP address, and its MAC address.
 *
 * Unlike the other handlers, this one generates a response body that is
 * cacheable, since these contents change rarely. This is indicated in the
 * Cache-Control response header. The header also sets an ETag header, as
 * described above. If the request contains an If-None-Match header whose
 * value matches the ETag, the header sends response status 304 ("Not
 * Modified"), with no body.
 *
 * This handler uses the private data pointer argument, which is set to
 * the netinfo structure initialized in main().
 */
err_t netinfo_handler(struct http *http, void *p)
{
	/*
	 * As above, use http_req() and http_resp() to get the objects
	 * that represent the current request and response.
	 */
	struct req *req = http_req(http);
	struct resp *resp = http_resp(http);
	netinfo_t *info;
	static char etag[ETAG_LEN] = {'\0'};
	char body[INFO_MAX_LEN];
	size_t body_len;
	err_t err;

	/*
	 * Cast the private data pointer to a pointer to an object of type
	 * struct netinfo.
	 *
	 * CAST_OBJ_NOTNULL() from picow_http/assertion.h asserts that p
	 * is not NULL, and that its 'magic' field has the value
	 * NETINFO_MAGIC. In a debug build, this is a safeguard against
	 * "wild pointer" errors.
	 *
	 See: https://slimhazard.gitlab.io/picow_http/group__assert.html#gaf2f178320dc4cf54496687d9afaa3377
	 */
	CAST_OBJ_NOTNULL(info, p, NETINFO_MAGIC);

	/* Initialize the ETag string. */
	if (etag[0] == '\0')
		set_etag(etag, info);

	/*
	 * The HTTP standard requires that ETag and Cache-Control headers
	 * are sent in response headers with status 304 as they would be
	 * for status 200. So we generate both of these before checking
	 * for an If-None-Match request header.
	 *
	 * See: https://www.rfc-editor.org/rfc/rfc9110#name-304-not-modified
	 */

	/*
	 * Set the ETag response header with http_resp_set_hdr(). In this
	 * case we explicitly set the lengths of both the header and the
	 * value. STRLEN_LTRL() is a convenience for getting the length
	 * of a literal string without using a hard-wired value.
	 *
	 * See:
	 * https://slimhazard.gitlab.io/picow_http/group__resp.html#gada5e398b617e954df42aff7f2a80c588
	 * https://slimhazard.gitlab.io/picow_http/group__util.html#ga3cc9a3f2bb13de555e52409196be0749
	 */
	if ((err = http_resp_set_hdr(resp, "ETag", STRLEN_LTRL("ETag"), etag,
								 ETAG_LEN - 1)) != ERR_OK)
	{
		HTTP_LOG_ERROR("Set header ETag failed: %d", err);
		return http_resp_err(http, HTTP_STATUS_INTERNAL_SERVER_ERROR);
	}

	/*
	 * Set the Cache-Control response header. Similar to above, except
	 * in this case we set the response to be cacheable for one hour.
	 */
	if ((err = http_resp_set_hdr_ltrl(resp, "Cache-Control",
									  "public, max-age=3600")) != ERR_OK)
	{
		HTTP_LOG_ERROR("Set header Cache-Control failed: %d", err);
		return http_resp_err(http, HTTP_STATUS_INTERNAL_SERVER_ERROR);
	}

	/*
	 * If the request has an If-None-Match header, and its value is
	 * equal to the hash string computed for the ETag, send a response
	 * with status 304 and no body.
	 *
	 * http_req_hdr_eq() returns true if the given header is present
	 * in the request, and its value is equal to the given value. It
	 * returns false if the header is not found in the request, or if
	 * the values do not match.
	 *
	 * See: https://slimhazard.gitlab.io/picow_http/group__req.html#ga30ad0922efc39acd7c31cc53dabf59c0
	 */
	if (http_req_hdr_eq(req, "If-None-Match", STRLEN_LTRL("If-None-Match"),
						etag, ETAG_LEN - 1))
	{
		/*
		 * Set status 304 with http_resp_set_status().
		 *
		 * See: https://slimhazard.gitlab.io/picow_http/group__resp.html#gaaa02fc68b02c0c8d0cbb304d0e46f0bb
		 */
		err = http_resp_set_status(resp, HTTP_STATUS_NOT_MODIFIED);
		if (err != ERR_OK)
		{
			HTTP_LOG_ERROR("Set status 304 failed: %d", err);
			return http_resp_err(http,
								 HTTP_STATUS_INTERNAL_SERVER_ERROR);
		}
		/*
		 * http_resp_send_hdr() sends the response
		 * header. Generally it is possible to also send a
		 * response body after calling http_resp_send_hdr(); but
		 * since we return from here, only the header is sent.
		 *
		 * See: https://slimhazard.gitlab.io/picow_http/group__resp.html#ga8131543f91511d845a4e657b0bead6fc
		 */
		return http_resp_send_hdr(http);
	}

	/*
	 * If we get here, no 304 response will be sent, so we send a
	 * response body with status 200. The code for sending a JSON
	 * response is similar to the code above for the rssi handler.
	 *
	 * Format the response body in the body array, and use the return
	 * value from snprintf() for the body length.
	 */
	body_len = snprintf(body, INFO_MAX_LEN, INFO_FMT, info->ip, info->mac);

	/*
	 * Set the Content-Length header with http_resp_set_len().
	 */
	if ((err = http_resp_set_len(resp, body_len)) != ERR_OK)
	{
		HTTP_LOG_ERROR("http_resp_set_len() failed: %d", err);
		return http_resp_err(http, HTTP_STATUS_INTERNAL_SERVER_ERROR);
	}

	/*
	 * Set the Content-Type header to "application/json" with
	 * http_resp_set_type_ltrl().
	 */
	if ((err = http_resp_set_type_ltrl(resp, "application/json")) != ERR_OK)
	{
		HTTP_LOG_ERROR("http_resp_set_type_ltrl() failed: %d", err);
		return http_resp_err(http, HTTP_STATUS_INTERNAL_SERVER_ERROR);
	}

	/*
	 * Send the response body, with the 'durable' parameter set to
	 * false, since the body is in a local array.
	 *
	 * As with the previous handlers, the default status is 200, and
	 * only the header is sent if the request method was HEAD.
	 */
	return http_resp_send_buf(http, body, body_len, false);
}
