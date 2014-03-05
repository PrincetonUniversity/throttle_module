#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

/* For inet_ntoa. */
#include <arpa/inet.h>

/* Required by event.h. */
#include <sys/time.h>

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <err.h>
#include <pthread.h>
#include <signal.h>
#include <sys/epoll.h>

/* Libevent. */
#include <event2/event.h>
#include <event2/thread.h>

#include "vnet_lib.h"
#include "queue.h"
#include "seq_queue.h"
#include "token_bucket.h"

#define POISON_MODE -1
#define EVENT_MODE 0
#define TB_MODE 1

int * rates;
int rate_array_size;

struct queue_root * tap_input_queue;
seq_queue tap_private_queue;

#define NUM_WORKERS 4

//note that for token buket, the buffer size has to be max_rate/(frequency). So max rate for now is: 10MBps
#define BUFF_SIZE (1024*10)

int gc = 0;
int gc1 = 0;
int total = 0;
int mask_g = -1;

struct event_base * main_base;


//struct event_base * main_base;
void on_read(evutil_socket_t fd, short ev, void *arg);
void on_write(evutil_socket_t fd, short ev, void *arg);

/**
 * A struct for client specific data, in this simple case the only
 * client specific data is the read event.
 */
struct client {
	int fd;
	struct event * ev_read;
	struct event * ev_write;
	struct client * other;
	int is_app;
	int closed;
	char buffer[BUFF_SIZE];
	char read_buffer[BUFF_SIZE];
	int off;
	int len;
	struct sockaddr_storage temp_addr;
	socklen_t addr_len;
	int gc;

	/// token buffer specific;
	struct token_bucket tb;
	int mode;	//can either be in event mode, or tb mode
	pthread_mutex_t * mutex;

	int rport; //unused right now
};


/**
 * Set a socket to non-blocking mode.
 */
int setnonblock_fd(int fd)
{
	int flags;

	flags = fcntl(fd, F_GETFL);
	if (flags < 0)
		return flags;
	flags |= O_NONBLOCK;
	if (fcntl(fd, F_SETFL, flags) < 0)
		return -1;

	return 0;
}


struct client * alloc_client(int fd){
	setnonblock_fd(fd);
	struct client * client = (struct client *)malloc(sizeof(struct client));
	client->fd = fd;
	client->closed = 0;
	client->off = 0;
	client->len = 0;

	//all events are created for all, addition will be done selectively
	client->ev_read = event_new(main_base,client->fd,EV_READ|EV_PERSIST,on_read,client);
	client->ev_write = event_new(main_base,client->fd,EV_WRITE|EV_PERSIST,on_write,client);

	return client;
}

void register_client_event(struct client * client){

	event_add(client->ev_read, NULL);

}

void close_free_client(struct client * client){
	close(client->fd);
	event_free(client->ev_read);
	event_free(client->ev_write);
	free(client);
}


void close_client(struct client * client){

	///////!!!!!!!!!!

	pthread_mutex_unlock(client->mutex);
	pthread_mutex_destroy(client->mutex);
	free(client->mutex);
	close_free_client(client->other);
	close_free_client(client);

	//printf("freed %u!!!!!!!!!!!!!!\n",client,client->other);
	//event_del(client->ev_read);

}


void on_write(evutil_socket_t fd, short ev, void *arg)
{
	struct client *client = (struct client *)arg;
	int wlen;

	pthread_mutex_lock(client->mutex);
	if (client->len>0)
	{

		if(client->is_app == 1)
			wlen = syscall(__NR_send_vsock,client->fd,client->buffer + client->off,client->len,MSG_NOSIGNAL,&client->other->temp_addr,client->other->addr_len);
		else
			wlen = send(client->fd,client->buffer + client->off, client->len,MSG_NOSIGNAL);

		if (wlen==-1 && errno==EAGAIN)
			wlen = 0;

		else if(wlen==-1)
		{
			perror("on_write");
			event_del(client->ev_write);
			if(client->other->len>0)
			{
				event_del(client->other->ev_write);
			}
			else
			{
				//either my read event or his write event
				event_del(client->ev_read);
			}
			close_client(client);
			goto OUT;

		}

		if(!client->is_app){

			//shaping omly supported for app side for now

			//update tokens
			drain_bytes(&client->other->tb,wlen);

			/*if(client->other->tb.tokens == 0)
			{
				client->other->mode = TB_MODE;
				event_del(client->ev_write);
				struct queue_head * curr = malloc(sizeof(struct queue_head));
				curr->entry = client->other;
				queue_put(curr,tap_input_queue);

			}*/
		}

		client->len -= wlen;
		client->off += wlen;

	}

	if(client->len==0)
	{
		//either my read event or his write event!!
		event_del(client->ev_write);
		event_add(client->other->ev_read, NULL);
	}

	UNLOCK:
	pthread_mutex_unlock(client->mutex);

	OUT:
	return;

}
/**
 * This function will be called by libevent when the client socket is
 * ready for reading.
 */
//we will set the threshold to a lower value
int on_read_tap(evutil_socket_t fd, void *arg){

	struct client *client = (struct client *)arg;
	client->addr_len = sizeof(client->temp_addr);

	//printf("read herees %d\n",client->is_app);   
	//if no space return!!
	int len, wlen, ret=0;
	int to_read =  sizeof(client->read_buffer);

	pthread_mutex_lock(client->mutex);
	if(client->mode == POISON_MODE)
	{
		printf("poisoned \n");
		close_client(client);
		ret = -1;
		goto OUT;
	}

	/*struct timeval curr;
	gettimeofday(&curr,NULL);
	printf("%ld\n",(curr.tv_sec - client->tb.start.tv_sec)*1000000 + (curr.tv_usec - client->tb.start.tv_usec));
	 */

	update_bytes(&client->tb);

	//if(allowed_bytes(&client->tb)>to_read)
	//printf("here to_read=%d %d\n",to_read,allowed_bytes(&client->tb));

	to_read = allowed_bytes(&client->tb)<to_read?allowed_bytes(&client->tb):to_read;

	//printf("on read_tap %d %d %d %d\n",mask_g,client->gc,to_read,client->tb.spent);
	//if(allowed_bytes(&client->tb)>to_read)
	//printf("here to_read=%d %d\n",to_read,allowed_bytes(&client->tb));

	if(client->is_app == 0)
	{
		len = recvfrom(fd,client->read_buffer,to_read,MSG_DONTWAIT,(struct sockaddr *)&client->temp_addr,&client->addr_len);
	}
	else
	{
		len = read(fd, client->read_buffer, to_read);
	}

	if (len < 0 && errno==EAGAIN)
	{
		//printf("Here in EAGAIN of the tap worker\n");
		ret = 1;
		//ret = 0;
		goto OUT_UNLOCK;
	}
	if (len == 0) {

		//if i'm the one who gets closed first

		//printf("%d\n",client->tb.spent);
		if(shutdown(client->other->fd,SHUT_WR)==-1 && errno == ENOTCONN)
		{

			if(client->other->mode==EVENT_MODE)
			{
				if(client->len==0)
					event_del(client->other->ev_read);
				else
					event_del(client->ev_write);
				client->other->closed = 1;

			}
			else
				client->other->mode==POISON_MODE;

		}

		/*
		//if(client->app){
			struct timeval curr;
			gettimeofday(&curr,NULL);
			printf("%ld\n",(curr.tv_sec - client->tb.start.tv_sec)*1000000 + (curr.tv_usec - client->tb.start.tv_usec));
		//}
		 //*/
		client->closed = 1;

		if(client->closed&&client->other->closed)
		{
			close_client(client);
			ret = -1;
			goto OUT;
		}

		ret = -1;
		goto OUT_UNLOCK;
	}
	else if (len < 0) {
		/* Some other error occurred, close the socket, remove
		 * the event and free the client structure. */
		printf("Socket failure, disconnecting client: not handled %d\n",
				errno);


		if(client->other->mode==EVENT_MODE) {

			if(client->len>0)
			{
				event_del(client->ev_write);
			}
			else
			{
				//either my read event or his write event
				event_del(client->other->ev_read);
			}

			close_client(client);
			ret = -1;
			goto OUT;
		}
		else
			client->other->mode = POISON_MODE;
		//printf("ERROR UNKOWN IN READING!!!!!!!!!!\n");

		//if(shutdown(client->other->fd,SHUT_WR)==-1 && errno == ENOTCONN)
		//close_client(client->other);

		ret = -1;
		goto OUT_UNLOCK;
	}

	/* XXX For the sake of simplicity we'll echo the data write
	 * back to the client.  Normally we shouldn't do this in a
	 * non-blocking app, we should queue the data and wait to be
	 * told that we can write.
	 */
	if(client->is_app == 0 && client->addr_len!=0)	//why addr_len!=0??
	{
		wlen = syscall(__NR_send_vsock,client->other->fd,client->read_buffer,len,MSG_NOSIGNAL,&client->temp_addr,client->addr_len);
	}
	else
	{
		wlen = send(client->other->fd,client->read_buffer,len,MSG_NOSIGNAL);
	}



	//printf("Sent wlen %d, len %d\n",wlen,len);
	if(wlen==-1 && errno==EPIPE)
	{
		printf("EPIPE %d!!!!!!!!!!\n",client->is_app);

		if(client->other->mode==EVENT_MODE) {

			if(client->len>0)
			{
				event_del(client->ev_write);
			}
			else
			{
				//either my read event or his write event
				event_del(client->other->ev_read);
			}

			close_client(client);
			ret = -1;
			goto OUT;
		}
		else
			client->other->mode = POISON_MODE;
		//printf("ERROR UNKOWN IN READING!!!!!!!!!!\n");

		//if(shutdown(client->other->fd,SHUT_WR)==-1 && errno == ENOTCONN)
		//close_client(client->other);

		ret = -1;
		goto OUT_UNLOCK;
	}
	else if (wlen==-1 && errno==EAGAIN)
		wlen = 0;
	else if(wlen <0)
	{
		perror("on_read");
		goto OUT_UNLOCK;
	}

	//update tokens
	if(wlen>=0)
		drain_bytes(&client->tb,wlen);

	//if(client->is_app)
	//printf("onread_tap %d %d %d %d\n",mask_g,client->gc,wlen,client->tb.spent);

	if (wlen < len) {
		/* We didn't write all our data.  If we had proper
		 * queueing/buffering setup, we'd finish off the write
		 * when told we can write again.  For this simple case
		 * we'll just lose the data that didn't make it in the
		 * write.
		 */
		//printf("not handled %d %d %d\n",to_read,wlen,client->tb.tokens);


		//this should work??!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!

		void * x = memcpy(client->other->buffer,client->read_buffer+wlen,len - wlen);

		if(x!=client->other->buffer)
			printf("Error in memcpy. Short write, not all data echoed back to client. wlen %d, len %d\n",wlen,len);

		client->other->len = len - wlen;
		client->other->off = 0;
		//printf("Copied %d bytes to buffer",len - wlen);
		//we can't read no more from the client, unless the others write makes space
		//event_del(client->ev_read);
		event_add(client->other->ev_write, NULL);

		//you don't have to do anything now, the write event will take care of it.
		ret = -1;
		goto OUT_UNLOCK;
	}

	OUT_UNLOCK:
	pthread_mutex_unlock(client->mutex);

	OUT:
	return ret;

}

void
on_read(evutil_socket_t fd, short ev, void *arg)
{

	struct client *client = (struct client *)arg;
	client->addr_len = sizeof(client->temp_addr);	


	int len, wlen;
	int to_read = sizeof(client->read_buffer);

	pthread_mutex_lock(client->mutex);

	if(client->is_app)
	{
		//get more tokens for the delay and run.
		update_bytes(&client->tb);
		to_read = allowed_bytes(&client->tb)<to_read?allowed_bytes(&client->tb):to_read;
		//printf("on read %d %d %d %d\n",mask_g,client->gc,to_read,client->tb.spent);
	}

	//only when reading from nic
	if(client->is_app == 0)
	{
		len = recvfrom(fd,client->read_buffer,to_read,MSG_DONTWAIT,(struct sockaddr *)&client->temp_addr,&client->addr_len);
	}
	else
	{

		len = read(fd, client->read_buffer, to_read);

	}

	if (len < 0 && errno==EAGAIN)
	{
		goto OUT_UNLOCK;
	}
	if (len == 0) {
		/* Client disconnected, remove the read event and the
		 * free the client structure. */
		if(shutdown(client->other->fd,SHUT_WR)==-1 && errno == ENOTCONN)
		{

			//when the tap closes it it should set it to closed.

			//if(client->is_app)
			//printf("%d\n",client->tb.spent);

			if(client->other->mode==EVENT_MODE)
			{
				if(client->len==0)
					event_del(client->other->ev_read);
				else
					event_del(client->ev_write);
				client->other->closed = 1;

			}
			else
				client->other->mode==POISON_MODE;

		}

		client->closed = 1;
		event_del(client->ev_read);

		//note that unless there is a connection break, this is the point where it will always end, since the last read event added will do this. the write event never closes anything unless it gets EPIPE.
		if(client->closed&&client->other->closed)
		{
			close_client(client);
			goto OUT;
		}

		goto OUT_UNLOCK;
	}
	else if (len < 0) {
		/* Some other error occurred, close the socket, remove
		 * the event and free the client structure. */
		printf("disconnected in on_read %d %d\n",client->closed,errno);
		event_del(client->ev_read);

		if(client->other->mode==EVENT_MODE)
		{							
			if(client->len>0)
			{
				event_del(client->ev_write);
			}
			else
			{
				//either my read event or his write event
				event_del(client->other->ev_read);

			}

			close_client(client);
			goto OUT;
		}
		else
			client->other->mode==POISON_MODE;

		goto OUT_UNLOCK;
	}


	//write the data, put into the buffer of other if it blocks
	if(client->is_app == 0 && client->addr_len!=0)	//why addr_len!=0??
	{
		wlen = syscall(__NR_send_vsock,client->other->fd,client->read_buffer,len,MSG_NOSIGNAL,&client->temp_addr,client->addr_len);
	}
	else
	{
		wlen = send(client->other->fd,client->read_buffer,len,MSG_NOSIGNAL);
	}

	/*if(client->is_app)
	{
		//get more tokens for the delay and run.
		printf("on read %d %d %d %d\n",mask_g,client->gc,to_read,client->tb.spent);
	}
	else
		//printf("on readi %d %d %d %d\n",mask_g,client->gc,wlen,client->tb.spent);
		printf("port: %d\n",client->other->rport);
	 */

	if(wlen==-1 && errno==EPIPE)
	{
		if(client->is_app)
			printf("EPIPE from client side in on_read\n",client->is_app);
		else
			printf("EPIPE from nic side in on_read\n",client->is_app);


		event_del(client->ev_read);

		if(client->other->mode==EVENT_MODE)
		{							
			if(client->len>0)
			{
				event_del(client->ev_write);
			}
			else
			{
				//either my read event or his write event
				event_del(client->other->ev_read);
			}
			close_client(client);
			goto OUT;
		}
		else
			client->other->mode==POISON_MODE;


		goto OUT_UNLOCK;
	}
	else if (wlen==-1 && errno==EAGAIN)
		wlen = 0;
	else if(wlen <0)
	{
		perror("on_read");
		goto OUT_UNLOCK;
	}

	if(wlen>=0)
		drain_bytes(&client->tb,wlen);


	//if(client->is_app)
	//printf("onread_tap1 %d %d %d %d\n",mask_g,client->gc,wlen,client->tb.spent);

	if (wlen < len) {
		/* We didn't write all our data.  If we had proper
		 * queueing/buffering setup, we'd finish off the write
		 * when told we can write again.  For this simple case
		 * we'll just lose the data that didn't make it in the
		 * write.
		 */

		//if(client->is_app)
		//printf("not supported on_write on app\n");

		void * x = memcpy(client->other->buffer,client->read_buffer+wlen,len - wlen);

		if(x!=client->other->buffer)
			printf("Error in memcpy. Short write, not all data echoed back to client. wlen %d, len %d\n",wlen,len);

		client->other->len = len - wlen;
		client->other->off = 0;
		//printf("Copied %d bytes to buffer",len - wlen);
		//we can't read no more from the client, unless the others write makes space

		event_del(client->ev_read);

		event_add(client->other->ev_write, NULL);



	}
	else if(client->is_app){

		//shaping omly supported for app side for now

		//printf("onread %d\n",wlen);

		//update tokens

		if(client->tb.tokens == 0)
		{
			client->mode = TB_MODE;
			event_del(client->ev_read);
			struct queue_head * curr = malloc(sizeof(struct queue_head));
			curr->entry = client;
			queue_put(curr,tap_input_queue);

		}
	}



	OUT_UNLOCK:
	pthread_mutex_unlock(client->mutex);

	OUT:
	return;
}

/**
 * This function will be called by libevent when there is a connection
 * ready to be accepted.

void
on_accept(int fd, short ev, void *arg)
{
	int client_fd;
	struct sockaddr_in client_addr;
	socklen_t client_len = sizeof(client_addr);
	struct client *client;


	client_fd = accept(fd, (struct sockaddr *)&client_addr, &client_len);
	if (client_fd == -1) {
		warn("accept failed");
		return;
	}

	if (setnonblock(client_fd) < 0)
		warn("failed to set client socket non-blocking");

	client = calloc(1, sizeof(*client));
	if (client == NULL)
		err(1, "malloc failed");

	event_set(&client->ev_read, client_fd, EV_READ|EV_PERSIST, on_read, 
			client);

	event_add(&client->ev_read, NULL);

	printf("Accepted connection from %s\n",
			inet_ntoa(client_addr.sin_addr));
}
 */

//unused
void set_client_port(struct client * client){
	struct client * client_nic = client;
	socklen_t len;
	struct sockaddr_storage addr;
	char ipstr[INET6_ADDRSTRLEN];
	int port;
	len = sizeof addr;

	if(client->is_app)
		client_nic = client->other;

	getpeername(client_nic->fd, (struct sockaddr*)&addr, &len);

	// deal with both IPv4 and IPv6:
	if (addr.ss_family == AF_INET) {
		struct sockaddr_in *s = (struct sockaddr_in *)&addr;
		port = ntohs(s->sin_port);
		inet_ntop(AF_INET, &s->sin_addr, ipstr, sizeof ipstr);
	} else { // AF_INET6
		struct sockaddr_in6 *s = (struct sockaddr_in6 *)&addr;
		port = ntohs(s->sin6_port);
		inet_ntop(AF_INET6, &s->sin6_addr, ipstr, sizeof ipstr);
	}

	client->rport = port;

}

void register_client(int fd_app, int fd_nic)  {
	struct client *client_app, *client_nic;
	//printf("mallocing\n");
	client_app = alloc_client(fd_app);
	client_nic = alloc_client(fd_nic);
	//printf("mallocing_ends, %u, %u\n",client_app,client_nic);
	if(client_app==NULL || client_nic==NULL)
	{
		printf("Error in malloc\n");
		exit(1);
	}

	client_app->other = client_nic;
	client_nic->other = client_app;

	client_app->is_app = 1;
	client_nic->is_app = 0;

	//printf("before event_new\n");
	client_app->gc = gc;
	client_nic->gc = gc;


	pthread_mutex_t * client_mutex =  (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
	pthread_mutex_init(client_mutex, NULL);
	client_app->mutex = client_mutex;
	client_nic->mutex = client_mutex;

	client_nic->mode = EVENT_MODE;	//0 is event mode

	//printf("port: %d\n",port);
	struct queue_head * curr = malloc(sizeof(struct queue_head));
	curr->entry = client_app;
	init_token_bucket(&client_app->tb,gc);
	client_app->mode = TB_MODE;
	queue_put(curr,tap_input_queue);
	register_client_event(client_nic);

	gc++;
	//struct timeval  curr;
	//gettimeofday(&curr,NULL);
	//printf("after eventnic_add gc=%d, time=%ld\n",gc,curr.tv_usec)
}


//check for multithreaded??
int get_sockets() {

	int num = 100;
	int val = 1;

	int fd_app[num],fd_nic[num];
	val = 0;
	//struct timeval  curr_start;
	//gettimeofday(&curr_start,NULL);
	val = syscall(__NR_get_vsock,0,fd_app,fd_nic,&num);

	//if(val<0)
	//break;
	//total += val;
	//printf("got val=%d, total=%d\n",val,total);
	int i =0;
	for(i=0;i<val;i++)
	{	
		//printf("registering %d %d\n",fd_app[i],fd_nic[i]);
		register_client(fd_app[i],fd_nic[i]);
	}

	struct timeval  curr;
	gettimeofday(&curr,NULL);


	return val;
	//if(val>0)
	//printf("gc=%d, registered %d socket(s), %ld, %ld, %ld\n", gc, val, curr_start.tv_usec, mid.tv_usec, curr.tv_usec);
	//printf("gc=%d, registered %d socket(s)\n", gc, val);

}

void * dispatcher(void * ptr )
{

	//evthread_use_pthreads();

	//printf("got dispatcher\n");
	int epfd_central = epoll_create(10);
	struct epoll_event ev, events[1];
	int nfds,i;

	ev.events = 0x0800;
	ev.data.fd = 0;
	if(epoll_ctl(epfd_central, EPOLL_CTL_ADD, -1, &ev)==-1){
		perror("epoll_ctl: failed");
	}

	struct timeval  curr;

	int num =0,c =0;

	while(1){

		//printf("waiting for event nfds\n");
		//gettimeofday(&curr,NULL);
		//printf("waiting for event nfds time=%ld\n",curr.tv_usec);
		nfds = epoll_wait(epfd_central,events,1,-1);
		//printf("got event\n");
		//gettimeofday(&curr,NULL);
		//printf("got for event nfds time=%ld\n",curr.tv_usec);
		//printf("got nfds=%d\n",nfds);

		//for(i=0;i<nfds;i++){

		//got an event, %0x, %d\n",events[i].events&0x0800,events[i].data.fd);
		//if( (events[i].events&0x0800)!=0 && events[i].data.fd==0 ){
		//printf("here\n");		
		//int ret = pthread_mutex_trylock(&accept_mutex_lock);
		//if(ret==0){
		//gettimeofday(&curr,NULL);
		//printf("syscall get_sockets %ld\n",curr.tv_usec);
		//c = get_sockets();
		get_sockets();
		/*if(c>0)
		{
			num += c;
			printf("here %d %d\n",mask_g,num);
		}*/
		//gettimeofday(&curr,NULL);
		//printf("syscall get_sockets_ends %ld\n",curr.tv_usec);
		//	pthread_mutex_unlock(&accept_mutex_lock);
		//}
		//}
		//}

	}
}

void * tap_worker(void * ptr){

	struct queue_head * curr;
	void * curr_client=NULL;
	while(1)
	{
		//you might want to sleep for smaller duration whe you have more entries
		struct timespec tim, tim2;
		tim.tv_sec = 0;
		tim.tv_nsec = (1000/(float)REFILL_FREQUENCY)*1000000L; //sleep for 20ms

		if(nanosleep(&tim , &tim2) < 0 )   
		{
			printf("Nano sleep system call failed \n");
			return NULL;
		}

		while(curr = queue_get(tap_input_queue))
		{
			//printf("got from global queue\n");
			enqueue_seq(&tap_private_queue,curr->entry);
			free(curr);
		}


		int limit = tap_private_queue.count, i=0;

		//printf("%d\n",limit);
		for(i=0;i<limit && (curr_client=dequeue_seq(&tap_private_queue));i++){
			struct client * client_app = (struct client *) curr_client;

			//printf("got from local queue\n");


			//if(client_app==0)
			//printf("yes\n");
			int ret = on_read_tap(client_app->fd,client_app);
			//if(client_app==0)
			//		printf("yes1\n");


			//do the sending part, and see if need to enqueue!!!!!!
			if(ret==0)
			{
				enqueue_seq(&tap_private_queue,curr_client);
			}
			else if(ret==1){

				client_app->mode = EVENT_MODE;
				event_add(client_app->ev_read,NULL);
				//add the event
			}
			//ret=-1 means do nothing, it's been deleted

		}
		//get from the global queue

		//process the local queue

	}

	return NULL;

}


void * event_worker(void * ptr){

	event_base_loop(main_base,EVLOOP_NO_EXIT_ON_EMPTY);

}

void mask_sigusr(){
	sigset_t x;
	sigemptyset (&x);
	sigaddset(&x, SIGUSR1);
	sigprocmask(SIG_BLOCK, &x, NULL);
}

void fill_rates(char * fname){

	FILE *file = fopen ( fname, "r" );
	int i = 0;
	
	rates = (int *) malloc(MAX_RATE_ARRAY_SIZE*sizeof(int));
	rate_array_size = MAX_RATE_ARRAY_SIZE;
	
	if ( file != NULL )
	{
		char line [ 128 ]; 
		while ( fgets ( line, sizeof line, file ) != NULL && i < MAX_RATE_ARRAY_SIZE) 
		{
			rates[i] = atoi(line);
			if(rates[i]==0)
			{
				printf("error in parsing the rates\n");
				exit(1);
			}
			
			i++;
		}
		rate_array_size = i;
		
		if(rate_array_size==MAX_RATE_ARRAY_SIZE)
			printf("warning the rate log file has filled up the whole array\n");
		
		fclose ( file );
	}
	else
	{
		perror ( fname ); 
	}
}

int
main(int argc, char **argv)
{

	//mask_sigusr();
	if(argc!=2)
	{
		printf("Usage: bmod logfile_name\n Log file entries should be in kbps\n");
		return 1;
	}
	
	int reg = syscall(__NR_vnet_register,"bmod");	
	unsigned long mask = 1;
	int x = 0;

	for(x=0;x<NUM_WORKERS-1;x++){

		if(fork()==0)
			break;
		mask = mask<<1;

		//pthread_setaffinity_np (fishing_thread,mask,sizeof(mask));
	}

	sched_setaffinity(0,sizeof(mask),&mask);

	int i  = evthread_use_pthreads();

	if(i!=0)
		printf("error in evthread_use_pthreads: %d\n",i);

	printf("worker started........\n");

	mask_g = mask;
	tap_input_queue = ALLOC_QUEUE_ROOT();
	init_queue_seq(&tap_private_queue);

	main_base = event_base_new();

	fill_rates(argv[1]);
	
	//after this point things will start running, ensure that entries are initialized before this point
	pthread_t fishing_thread;
	int err = pthread_create( &fishing_thread, NULL, &event_worker, NULL);

	pthread_t tap_thread;
	err = pthread_create( &tap_thread, NULL, &tap_worker, NULL);

	dispatcher(NULL);
	
	//resources are freed automatically after this is closed, since the dispacthed is an infinite loop there is no point of freeing things after this point.
	printf("Server shutdown.\n");

	return 0;
}