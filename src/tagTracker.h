#ifndef TAG_TRACKER_H
#define TAG_TRACKER_H

#include "tag.h"

char * getTagItemString(int type, char * string);

void removeTagItemString(int type, char * string);

int getNumberOfTagItems(int type);

void printMemorySavedByTagTracker();

#endif
