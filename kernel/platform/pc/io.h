/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef PC_IO_H
#define PC_IO_H

unsigned char port_byte_in(unsigned short port);
void port_byte_out(unsigned short port, unsigned char data);

#endif
