
/*	queue.c

	Implementation of a FIFO queue abstract data type.

	by: Steven Skiena
	begun: March 27, 2002
 */


/*
Copyright 2003 by Steven S. Skiena; all rights reserved. 

Permission is granted for use in non-commerical applications
provided this copyright notice remains intact and unchanged.

This program appears in my book:

"Programming Challenges: The Programming Contest Training Manual"
by Steven Skiena and Miguel Revilla, Springer-Verlag, New York 2003.

See our website www.programming-challenges.com for additional information.

This book can be ordered from Amazon.com at

http://www.amazon.com/exec/obidos/ASIN/0387001638/thealgorithmrepo/

 */
#include <stdio.h>
#include "seq_queue.h"


void init_queue_seq(seq_queue *q)
{
	if(q==NULL)
	{
		printf("Warning: init_queue_seq null\n");
		return;
	}
	q->first = 0;
	q->last = QUEUESIZE-1;
	q->count = 0;
}

void enqueue_seq(seq_queue *q, void * x)
{

	if(q==NULL)
	{
		printf("Warning: enqueue_seq null\n");
		return;
	}

	if (q->count >= QUEUESIZE)
		printf("Warning: queue overflow enqueue x=%d\n",x);
	else {
		q->last = (q->last+1) % QUEUESIZE;
		q->q[ q->last ] = x;    
		q->count = q->count + 1;
	}
}

void * dequeue_seq(seq_queue *q)
{
	if(q==NULL)
	{
		printf("Warning: dequeue_seq null\n");
		return;
	}

	void * x=NULL;

	if (q->count <= 0) 
		return NULL;
	else {
		x = q->q[ q->first ];
		q->first = (q->first+1) % QUEUESIZE;
		q->count = q->count - 1;
	}

	return(x);
}

int empty_seq(seq_queue *q)
{

	if(q==NULL)
	{
		printf("Warning: empty_seq null\n");
		return;
	}
	
	if (q->count <= 0) return (1);
	else return (0);
}

/*
print_queue(seq_queue *q)
{
        int i,j;

        i=q->first; 

        while (i != q->last) {
                printf("%c ",q->q[i]);
                i = (i+1) % QUEUESIZE;
        }

        printf("%2d ",q->q[i]);
        printf("\n");
}*/


