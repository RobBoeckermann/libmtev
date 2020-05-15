/*
 * Copyright (c) 2016, Circonus, Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above
 *      copyright notice, this list of conditions and the following
 *      disclaimer in the documentation and/or other materials provided
 *      with the distribution.
 *    * Neither the name Circonus, Inc. nor the names
 *      of its contributors may be used to endorse or promote products
 *      derived from this software without specific prior written
 *      permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#ifndef UTILS_MTEV_UUID_PARSE_H
#define UTILS_MTEV_UUID_PARSE_H

#include <mtev_defines.h>
#include <mtev_uuid.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 \fn int mtev_uuid_parse(const char *in, uuid_t uu)
 \brief Parse "in" in UUID format into "uu".
 \return 0 on success, non-zero on parse error

 Follows the same semantics of uuid_parse from libuuid
*/
API_EXPORT(int)
  mtev_uuid_parse(const char *in, uuid_t uu);

/*!
  \fn void mtev_uuid_unparse_lower(const uuid_t uu, char *out)
  \brief Unparse "uu" into "out".

  Follows the same semantics of uuid_unparse_lower from libuuid.

  There is no bounds checking of "out", caller must ensure that "out"
  is at least UUID_STR_LEN in size.  This also does not NULL terminate
  "out".  That is also up to the caller.
*/
API_EXPORT(void)
  mtev_uuid_unparse_lower(const uuid_t uu, char *out);

/*!
  \fn void mtev_uuid_unparse_upper(const uuid_t uu, char *out)
  \brief Unparse "uu" into "out".

  Follows the same semantics of uuid_unparse_upper from libuuid.

  There is no bounds checking of "out", caller must ensure that "out"
  is at least UUID_STR_LEN in size.  This also does not NULL terminate
  "out".  That is also up to the caller.
*/
API_EXPORT(void)
  mtev_uuid_unparse_upper(const uuid_t uu, char *out);

/*!
  \fn void mtev_uuid_unparse(const uuid_t uu, char *out)
  \brief Unparse "uu" into "out".

  Follows the same semantics of uuid_unparse_lower from libuuid.

  There is no bounds checking of "out", caller must ensure that "out"
  is at least UUID_STR_LEN in size.  This also does not NULL terminate
  "out".  That is also up to the caller.
*/
API_EXPORT(void)
  mtev_uuid_unparse(const uuid_t uu, char *out);

#ifdef __cplusplus
}
#endif

#endif
