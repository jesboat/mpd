#include "tagTracker.h"

#include "list.h"

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
		tagLists[type] = makeList(NULL);
	}

	if((node = findNodeInList(tagLists[type], string))) {
		(*((int *)node->data))++;
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

	if(tagLists[type] == NULL) return;

	if((node = findNodeInList(tagLists[type], string))) {
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
