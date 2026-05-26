#ifndef SERIAL1_MANAGER_H
#define SERIAL1_MANAGER_H

#include <stdint.h>
#include <stddef.h>

#define SERIAL1_BUFFER_SIZE 256

void init_serial1();
void loop_serial1();                  // call in main loop
bool serial1_read_line(char* buf, size_t len);  // non-blocking line read
uint16_t serial1_available();          // bytes in buffer

#endif