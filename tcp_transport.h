#ifndef _TCP_TRANSPORT_H
#define _TCP_TRANSPORT_H

int tcp_wait_accept(struct node *node);
void tcp_listen(struct node *node, int port);
bool tcp_complete_connect(struct node *node);
bool tcp_connect(struct node *node, int port);
void tcp_wait_connect(struct node *node, int port);
void tcp_read(int fd, void *buf, int len);
void tcp_write(int fd, void *buf, int len);

#endif /* _TCP_TRANSPORT_H */
