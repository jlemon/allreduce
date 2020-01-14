int mesh_join(void);
void mesh_init(void);
struct node *mesh_get_node(int rank);
struct node *mesh_match_addr(struct sockaddr_storage *ss, socklen_t addrlen);

