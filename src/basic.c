/*
    basic.c - Basic Authorization

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/*********************************** Code *************************************/
/*
    Parse the 'Authorization' header and the server 'Www-Authenticate' header
 */
PUBLIC int httpBasicParse(HttpStream *stream, cchar **username, cchar **password)
{
    HttpRx  *rx;
    char    *decoded, *cp;

    rx = stream->rx;
    if (password) {
        *password = NULL;
    }
    if (username) {
        *username = NULL;
    }
    if (!rx->authDetails) {
        return 0;
    }
    if ((decoded = mprDecode64(rx->authDetails)) == 0) {
        return MPR_ERR_BAD_FORMAT;
    }
    if ((cp = strchr(decoded, ':')) != 0) {
        *cp++ = '\0';
    }
    stream->encoded = 0;
    if (decoded && *decoded == '\0') {
        return MPR_ERR_BAD_FORMAT;
    }
    if (username) {
        *username = sclone(decoded);
    }
    if (cp && *cp == '\0') {
        return MPR_ERR_BAD_FORMAT;
    }
    if (password) {
        *password = sclone(cp);
    }
    return 0;
}


/*
    Respond to the request by asking for a login
    Only called if not logged in
 */
PUBLIC void httpBasicLogin(HttpStream *stream)
{
    HttpAuth    *auth;

    auth = stream->rx->route->auth;
    if (auth->loginPage && !sends(stream->rx->referrer, auth->loginPage)) {
        httpRedirect(stream, HTTP_CODE_MOVED_TEMPORARILY, auth->loginPage);
    } else {
        httpSetHeader(stream, "WWW-Authenticate", "Basic realm=\"%s\"", auth->realm);
        httpError(stream, HTTP_CODE_UNAUTHORIZED, "Access Denied. Login required");
        httpLog(stream->trace, "auth.basic.error", "error", "msg:'Access denied, Login required'");
    }
}


/*
    Add the 'Authorization' header for authenticated requests
    NOTE: Can do this without first getting a 401 response
 */
PUBLIC bool httpBasicSetHeaders(HttpStream *stream, cchar *username, cchar *password)
{
    httpAddHeader(stream, "Authorization", "basic %s", mprEncode64(sfmt("%s:%s", username, password)));
    return 1;
}


/*
    Copyright (c) Embedthis Software. All Rights Reserved.
    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.
 */
