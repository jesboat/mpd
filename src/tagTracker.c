#include "tagTracker.h"

#include "list.h"
#include "log.h"

#include <assert.h>

static List * tagLists[TAG_NUM_OF_ITEM_TYPES] =
{
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL
};

char * getTagItemString(int type, char * string) {
	ListNode * node;
	
	if(tagLists[type] == NULL) {
		tagLists[type] = makeList(free);
	}

	if((node = findNodeInList(tagLists[type], string))) {
		(*((int *)node->data))++;
		printf("%s: %i\n", string, *((int *)node->data));
	}
	else {
		int * intPtr = malloc(sizeof(int));
		*intPtr = 1;
		node = insertInList(tagLists[type], string, intPtr);
	}

	return node->key;
}

void removeTagItemString(int type, char * string) {
	ListNode * node;

	assert(tagLists[type]);
	assert(string);
	if(tagLists[type] == NULL) return;

	node = findNodeInList(tagLists[type], string);
	assert(node);
	if(node) {
		int * countPtr = node->data;
		*countPtr--;
		if(*countPtr <= 0) deleteNodeFromList(tagLists[type], node);
	}

	if(tagLists[type]->numberOfNodes == 0) {
		freeList(tagLists[type]);
		tagLists[type] = NULL;
	}
}

int getNumberOfTagItems(int type) {
	if(tagLists[type] == NULL) return 0;

	return tagLists[type]->numberOfNodes;
}

void printMemorySavedByTagTracker() {
	int i;
	ListNode * node;
	size_t sum = 0;

	for(i = 0; i < TAG_NUM_OF_ITEM_TYPES; i++) {
		if(!tagLists[i]) continue;

		sum -= sizeof(List);
		
		node = tagLists[i]->firstNode;

		while(node != NULL) {
			sum -= sizeof(ListNode);
			sum -= sizeof(int);
			sum -= sizeof(node->key);
			sum += (strlen(node->key)+1)*(*((int *)node->data));
			node = node->nextNode;
		}
	}

	DEBUG("saved memory: %li\n", (long)sum);
}
