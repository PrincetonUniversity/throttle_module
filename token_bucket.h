/* Sharvanath: Implementation of token bucket for shaping module */
#include <time.h>

#define MAX_RATE_ARRAY_SIZE 1000
#define RATE_UNIT	128 //kbit/s
#define DEFAULT_RATE (1024*300) //300KBps
#define REFILL_FREQUENCY 100	//try sending FREQUENCY times per second


extern int * rates;
extern int rate_array_size;

struct token_bucket {

	struct timeval start; //when did the connection start
	struct timeval last_refill;
	long long int tokens;
	long long int rate; //in bytes per second
	long long int spent;
	int start_index;

};

void init_token_bucket(struct token_bucket * tb, int seed){
	if(tb==NULL)
	{
		perror("tb is NULL in init_token_bucket\n");
	}
	gettimeofday(&tb->start,NULL);
	gettimeofday(&tb->last_refill,NULL);
	tb->tokens = 0;
	tb->spent = 0;
	tb->rate = DEFAULT_RATE;

	srand(seed);
	
	if(rate_array_size){
		tb->start_index = rand()%rate_array_size;
	}
	//printf("%d %d\n",seed,tb->start_index);
}

int allowed_bytes(struct token_bucket * tb){
	return tb->tokens;
}

int drain_bytes(struct token_bucket * tb, int bytes){
	tb->tokens -= bytes; 
	tb->spent += bytes;
}

int update_bytes(struct token_bucket * tb){
	struct timeval curr;
	long int us_since_last_refill;
	long int us_since_beg;
	int s_since_beg;

	gettimeofday(&curr,NULL);
	if(rate_array_size){
		us_since_beg = (curr.tv_sec - tb->start.tv_sec) * 1000000 + (curr.tv_usec - tb->start.tv_usec);
		s_since_beg = us_since_beg/1000000;
		tb->rate = rates[(s_since_beg + tb->start_index)%rate_array_size]*RATE_UNIT;
	}
	//printf("tb->rate: %d\n",tb->rate);

	us_since_last_refill = (curr.tv_sec - tb->last_refill.tv_sec) * 1000000 + (curr.tv_usec - tb->last_refill.tv_usec) ;

	tb->tokens += (us_since_last_refill*tb->rate)/1000000; // number of tokens accumulated

	//if(us_since_beg>3000000)
	//printf("This connection is good and long for shaping: %d\n",us_since_beg);

	if(us_since_last_refill*tb->rate>1000000)	//don't penalize if not refilled
		gettimeofday(&tb->last_refill,NULL);

	//if(tb->tokens<=0)
	//printf("This should be really rare\n");

	return tb->tokens;
}

/*
int update_bytes(struct token_bucket * tb){
	struct timeval curr;
	long int us_since_last_refill;
	long int us_since_beg;
	int s_since_beg;

	gettimeofday(&curr,NULL);
	us_since_beg = (curr.tv_sec - tb->start.tv_sec) * 1000000 + (curr.tv_usec - tb->start.tv_usec);
	s_since_beg = us_since_beg/1000000;
	tb->rate = rates[(s_since_beg + tb->start_index)%rate_array_size]*RATE_UNIT;
	//printf("tb->rate: %d\n",tb->rate);

	us_since_last_refill = (curr.tv_sec - tb->last_refill.tv_sec) * 1000000 + (curr.tv_usec - tb->last_refill.tv_usec) ;

	tb->tokens += (us_since_last_refill*tb->rate)/1000000; // number of tokens accumulated

	//if(us_since_beg>3000000)
		//printf("This connection is good and long for shaping: %d\n",us_since_beg);

	if(us_since_last_refill*tb->rate>1000000)	//don't penalize if not refilled
			gettimeofday(&tb->last_refill,NULL);

	//if(tb->tokens<=0)
		//printf("This should be really rare\n");

	return tb->tokens;
}
 */

