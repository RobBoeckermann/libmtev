# Listeners

Network listeners and their services are specified via configuration.

##### application.conf (snippet)

```xml
  <listeners>
    <sslconfig>
      <optional_no_ca>false</optional_no_ca>
      <certificate_file>/path/to/server.crt</certificate_file>
      <key_file>/path/to/server.key</key_file>
      <ca_chain>/path/to/ca.crt</ca_chain>
      <ca_accept/>
      <layer>tlsv1:&gt;=tlsv1.2,cipher_server_preference</layer>
      <ciphers>EECDH+AES128+AESGCM:EDH+AES128+AESGCM:!DSS</ciphers>
    </sslconfig>
    <consoles type="mtev_console" require_env="MTEV_CONTROL">
      <listener address="127.0.0.1" port="32322">
        <config>
          <line_protocol>telnet</line_protocol>
        </config>
      </listener>
    </consoles>
    <web type="control_dispatch" address="*">
      <config>
        <idle_timeout>30000</idle_timeout>
        <document_root>/path/to/docroot</document_root>
      </config>
      <listener port="80" />
      <listener port="443" ssl="on" />
    </web>
  </listeners>
```

This example demonstrates many powerful concepts of the libmtev configuration system.
There are three listener stanzas nested above and we'll walk through each.  The first
is the `<listener address="127.0.0.1" port="32322">`.  With this, you can telnet to
127.0.0.1 port 32322 and talk with your libmtev application.  The console is extensible
so you can add application-specific command, control, and interrogation capabilities.

This listener has a `<config>` stanza underneath it that sets `line_protocol` to `telnet`.
`line_protocol` is a configuration option for listeners of type `mtev_console`.  You'll
note that the listener's `type` attribute was actually set in a parent node.  Most
systems in libmtev will recusively merge from ancestors down to the a specimen node
and use that result.  Here `type` is simply an attribute, so merging is just replacing.
This node also has an `sslconfig`, but it doesn't use it, so we'll ignore that for now.
The `require_env` attribute requires the `MTEV_CONTROL` environment variable to be set
for this listener to be active; if unspecified, it is active.

The next two listener stanzas are for port 80 and 443.  They are in a `web` node that has
both `type` and `address` attributes set (those are inherited by the listeners).  The
`config` node (child of `web`) and the `sslconfig` node (child of `listeners`) are also
inherited into the `listener` nodes.  The `config` is arbitrary and passed into the listener.
The `sslconfig` is passed into the ssl subsystem and is uniform across all listener types.

The following attributes are supported for listeners:

 * ##### type

   The type of listener simply references a named eventer callback in the system (one
   registered with `eventer_name_callback(...)`.  libmtev support four built-in listener
   types: `http_rest_api`, `mtev_wire_rest_api/1.0`, `control_dispatch`, and `mtev_console`.
   Applications can arbitrarily extend the system by naming callbacks.

 * ##### require_env

   This optionally requires conditions around an environment variable. See 
   [`require_env`](README.md#requireenv).

 * ##### address

   The address is either a filesystem path (AF_UNIX), an IPv4 address or an IPv6 address.
   The type is intuited from the input string.  If the special string `*` or `inet:*` is used,
   then the IPv4 `in_addr_any` address is used for listening. IF `inet6:*` is used, then the
   IPv6 `in_addr_any` address is used for listening.

 * ##### port

   Specifies the port on which to listen.  This has no meaning for AF_UNIX-based addresses.

 * ##### ssl

   If the value here is `on`, then the socket passes through SSL negotiation before handed
   to the underlying system driving the specified listener type.

 * ##### fanout

   If the value here is `on`, the new events created for accepted connections will be fanned
   out across threads in the event pool owning the listening socket (usually the default
   event pool).  A different pool can be selected by additionally supplying `fanout_pool`.

 * ##### fanout_pool

   If `fanout` is `on`, this will select a named pool on which to distribute new connection
   events.  The value of this attribute should be the name of an event pool.  If not pool
   exists with the specified name, the pool containing the listening event will be used.

 * ##### accept_thread

   If `accept_thread` is `on`, a new dedicated thread will be spawned to handle accepting
   new connections in a blocking fashion.

 * ##### no_delay

   If `no_delay` is `off` or `false`, then `TCP_NODELAY` will not be activated
   on the accepted socket. The default is `on`.

 * ##### idle_timeout

   Specifies a time in milliseconds afterwhich if the connection remains idle (no read or write traffic) it will be terminated.
   The protocol driver must cooperate programmatically to inform the system of such activity; the `mtev_console` and `http` protocols do this.


   Each listener can access the `config` passed to it; see type-specific documentation for other config keys.

### sslconfig

The ssl config allow specification of many aspects of how SSL is negotiated with
connecting clients.  SSL config supports the follwing keys:

 * ##### layer

   This specifies the SSL protocol options we present and is the form `<protocol>[:<option>,[<option>[,...]]]`.
   Options may be negated with an antecedent `!`.  Tokens are matched case-insensitively.

   Protocols supported (depending on openssl): `SSLv2`, `SSLv3`, `TLSv1`, `TLSv1.1`, `TLSv1.2`, `TLSv1.3`

   Options supported (depending on openssl): `SSLv2`, `SSLv3`, `TLSv1`, `TLSv1.1`, `TLSv1.2`, `TLSv1.3`, `cipher_server_preference`

   TLS version options can be prefixed with `=`, `>=`, or `<=`. The default layer string is `tlsv1:>=tls1.2,cipher_server_preference`

 * ##### certificate_file or certificate

   Specifies the path to a PEM encoded certificate file.
   You may include the PEM block inline.

 * ##### key_file or key

   Specifies the path to a PEM encoded key file.  It must not be encrypted with a password.
   You may include the PEM block inline.

 * ##### ca_chain or ca_file

   Specifies the CA verification list file (PEM encoded) that should be used to certificates from the client.
   You may include the PEM block inline.

 * ##### ca_accept

   Specifies the CA certificates file (PEM encoded) that should be advertised to clients, if not specified `ca_file` is used.
   You may include the PEM block inline.

 * ##### crl

   Specifies a PEM encoded certificate revocation list file.  If not specified, no revocation is enforced.

 * ##### ciphers

   Specifies which ciphers should be supported, expressed in the OpenSSL cipher
   list format.  Check the OpenSSL manual for more details.  If not specified, the
   default ciphers supported by the OpenSSL library are used.

 * ##### npn or alpn

   Specifies which NPN (next-protocol-negotiation) to offer.  If omitted, `h2` is used and the http2
   protocol is exposed.  Specifying `none` will disable this NPN/ALPN registration.

 * ##### optional_no_ca

   If set to "true", no checks on the validate of the signing CA will be performed. The default is "false".

 * ##### ignore_dates

   If set to "true", expired or future certificates will be considered valid. The default is "false".
