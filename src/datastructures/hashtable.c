#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <string.h>

typedef struct entry_s {
	char *key;
	char *value;
	struct entry_s *next;
} entry_t;

typedef struct {
	int size;
	entry_t **table;
} hashtable_t;


/* Create a new hashtable. */
hashtable_t *ht_create (int size) {
	int i;
	hashtable_t *hashtable = malloc(sizeof(hashtable_t));
	hashtable->table = malloc(sizeof(entry_t*) * size);
	for( i = 0; i < size; i++ )
		hashtable->table[i] = NULL;
	
	hashtable->size = size;
	return hashtable;	
}

/* Hash a string for a particular hash table. */
int ht_hash(hashtable_t *hashtable, char *key ) {
	unsigned long int hashval=0;
	int i;
	
	for (i=0; hashval < ULONG_MAX && i < strlen( key ); i++)
		hashval = (hashval << 8) + key[i];
	
	return hashval % hashtable->size;
}

/* Create a key-value pair. */
entry_t *ht_newpair(char *key, char *value, entry_t *next) {
	entry_t *newpair = malloc( sizeof(entry_t) );
	newpair->key = strdup(key);
	newpair->value = strdup(value);
	newpair->next = next;
	return newpair;
}

/* Insert a key-value pair into a hash table. */
void ht_set( hashtable_t *hashtable, char *key, char *value ) {
	int bin = ht_hash( hashtable, key );
	entry_t *next = hashtable->table[bin];
	entry_t *last, *newpair;
	
	while(next && next->key && strcmp(key, next->key) > 0) {
		last = next;
		next = next->next;
	}
	
	if (!hashtable->table[bin])
		hashtable->table[bin] = ht_newpair(key, value, NULL);
	else if (!next || !next->key || strcmp(key, next->key)==0)
		last->next = ht_newpair(key, value, next);
	else {
		free (next->value);
		next->value = strdup(value);
	}
}

/* Retrieve a key-value pair from a hash table. */
char *ht_get( hashtable_t *hashtable, char *key ) {
	entry_t *pair = hashtable->table[ht_hash( hashtable, key )];
	while( pair && pair->key && strcmp(key, pair->key) > 0 )
		pair = pair->next;

	if (!pair || !pair->key)
		return NULL;
	else
		return pair->value;
}

void main() {}
