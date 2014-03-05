all:
	gcc workers.c queue.c seq_queue.c -levent -levent_pthreads -lpthread -o bmod  
clean:
	rm bmod
