#ifndef CGDB_NATIVE_CONSOLE_PROTOCOL_H
#define CGDB_NATIVE_CONSOLE_PROTOCOL_H
/* Private CGDB -> console host protocol. Three-byte header: type, then
 * unsigned little-endian payload length. Output is unframed terminal data. */
#define CGDB_CONSOLE_INPUT 1
#define CGDB_CONSOLE_RESIZE 2
#define CGDB_CONSOLE_MAX_PAYLOAD 4096
#endif
