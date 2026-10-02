/*
   Copyright (C) gnbdev

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef GNB_ADDRESS_H
#define GNB_ADDRESS_H

#include "gnb_address_type.h"

#define GNB_ADDRESS_LIFE_TIME_TS_SEC   120
#define GNB_ADDRESS_AVAILABLE(address,now_sec) (now_sec - address->ts_sec) < GNB_ADDRESS_LIFE_TIME_TS_SEC ? 1:0

/*
当 gnb_addr_secure 为非零时将不输出的完整的ip地址
*/
extern uint8_t gnb_addr_secure;
static char gnb_address_static_str1[GNB_IP6_PORT_STRING_SIZE];
static char gnb_address_static_str2[GNB_IP6_PORT_STRING_SIZE];

unsigned long long gnb_ntohll(unsigned long long val);
unsigned long long gnb_htonll(unsigned long long val);
char get_netmask_class(uint32_t addr4);

char * gnb_in4_addr_str(void *byte4,  char *dest, uint8_t addr_secure);
char * gnb_in6_addr_str(void *byte16, char *dest, uint8_t addr_secure);
#define GNB_IN4_ADDR_STR1(byte4)  gnb_in4_addr_str(byte4,gnb_address_static_str1,gnb_addr_secure)
#define GNB_IN4_ADDR_STR2(byte4)  gnb_in4_addr_str(byte4,gnb_address_static_str2,gnb_addr_secure)
#define GNB_IN6_ADDR_STR1(byte16) gnb_in6_addr_str(byte16,gnb_address_static_str1,gnb_addr_secure)
#define GNB_IN6_ADDR_STR2(byte16) gnb_in6_addr_str(byte16,gnb_address_static_str2,gnb_addr_secure)
#define GNB_IN4_ADDR_PLAINTEXT1(byte4)  gnb_in4_addr_str(byte4,gnb_address_static_str1,0)
#define GNB_IN4_ADDR_PLAINTEXT2(byte4)  gnb_in4_addr_str(byte4,gnb_address_static_str2,0)
#define GNB_IN6_ADDR_PLAINTEXT1(byte16) gnb_in6_addr_str(byte16,gnb_address_static_str1,0)
#define GNB_IN6_ADDR_PLAINTEXT2(byte16) gnb_in6_addr_str(byte16,gnb_address_static_str2,0)

char * gnb_address_port_str(gnb_address_t *address, char *dest, uint8_t addr_secure);
#define GNB_ADDRESS_PORT_STR1(address)  gnb_address_port_str(address,gnb_address_static_str1,gnb_addr_secure)
#define GNB_ADDRESS_PORT_STR2(address)  gnb_address_port_str(address,gnb_address_static_str2,gnb_addr_secure)

char * gnb_sockaddr_in6_str(struct sockaddr_in6 *in6, char *dest, uint8_t addr_secure);
char * gnb_sockaddr_in4_str(struct sockaddr_in *in, char *dest, uint8_t addr_secure);
#define GNB_SOCKADDR_IN6_STR1(in6) gnb_sockaddr_in6_str(in6,gnb_address_static_str1,gnb_addr_secure)
#define GNB_SOCKADDR_IN6_STR2(in6) gnb_sockaddr_in6_str(in6,gnb_address_static_str2,gnb_addr_secure)
#define GNB_SOCKADDR_IN4_STR1(in4) gnb_sockaddr_in4_str(in4,gnb_address_static_str1,gnb_addr_secure)
#define GNB_SOCKADDR_IN4_STR2(in4) gnb_sockaddr_in4_str(in4,gnb_address_static_str2,gnb_addr_secure)

char * gnb_sockaddr_in6_port_str(struct sockaddr_in6 *in6, char *dest, uint8_t addr_secure);
char * gnb_sockaddr_in4_port_str(struct sockaddr_in  *in,  char *dest, uint8_t addr_secure);
#define GNB_SOCKADDR_IN6_PORT_STR1(in6) gnb_sockaddr_in6_port_str(in6,gnb_address_static_str1,gnb_addr_secure)
#define GNB_SOCKADDR_IN6_PORT_STR2(in6) gnb_sockaddr_in6_port_str(in6,gnb_address_static_str2,gnb_addr_secure)
#define GNB_SOCKADDR_IN4_PORT_STR1(in)  gnb_sockaddr_in4_port_str(in, gnb_address_static_str1,gnb_addr_secure)
#define GNB_SOCKADDR_IN4_PORT_STR2(in)  gnb_sockaddr_in4_port_str(in, gnb_address_static_str2,gnb_addr_secure)

char * gnb_sockaddress_port_str(gnb_sockaddress_t *sockaddress, char *dest, uint8_t addr_secure);
#define GNB_SOCKADDRESS_PORT_STR1(sockaddress) gnb_sockaddress_port_str(sockaddress,gnb_address_static_str1,gnb_addr_secure)
#define GNB_SOCKADDRESS_PORT_STR2(sockaddress) gnb_sockaddress_port_str(sockaddress,gnb_address_static_str2,gnb_addr_secure)

int gnb_cmp_sockaddr_in6(struct sockaddr_in6 *in1, struct sockaddr_in6 *in2);
int gnb_cmp_sockaddr_in(struct sockaddr_in *in1, struct sockaddr_in *in2);

/*
只判断 prefixlen=96 的子网
*/
int gnb_determine_subnet6_prefixlen96(struct in6_addr addr6_a, struct in6_addr addr6_b);
int gnb_determine_subnet4(struct in_addr addr4_a, struct in_addr addr4_b, struct in_addr netmask);

//返回 -1 没有找到,  >0 为 address 在 list 中的索引编号
int gnb_address_list_find(gnb_address_list_t *address_list, gnb_address_t *address);

//把 address 放进列表中一个空的项里
void gnb_address_list_update(gnb_address_list_t *address_list, gnb_address_t *address);
void gnb_address_list3_fifo(gnb_address_list_t *address_list, gnb_address_t *address);

#define GNB_PROTOCOL_UDP   0x1
#define GNB_PROTOCOL_TCP   0x2
void gnb_sockaddress6_set(gnb_sockaddress_t *sockaddress, int protocol, const char *host, int port);
void gnb_sockaddress4_set(gnb_sockaddress_t *sockaddress, int protocol, const char *host, int port);

void gnb_address6_set(gnb_address_t *address, struct sockaddr_in6 *in6);
void gnb_address4_set(gnb_address_t *address, struct sockaddr_in *in);


//unuse
gnb_address_t gnb_get_address4_from_string(const char *sockaddress4_string);

#endif
