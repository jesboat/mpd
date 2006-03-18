/* the Music Player Daemon (MPD)
 * (c)2003-2004 by Warren Dukes (shank@mercury.chem.pitt.edu)
 * This project's homepage is: http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "permission.h"

#include "conf.h"
#include "list.h"
#include "log.h"

#include <string.h>

#define PERMISSION_PASSWORD_CHAR	"@"
#define PERMISSION_SEPERATOR		","

#define PERMISSION_READ_STRING		"read"
#define PERMISSION_ADD_STRING		"add"
#define PERMISSION_CONTROL_STRING	"control"
#define PERMISSION_ADMIN_STRING		"admin"

List * permission_passwords;

unsigned int permission_default;

unsigned int parsePermissions(char * string) {
	unsigned int permission = 0;
	char * temp;
	char * tok;

	if(!string) return 0;

	temp = strtok_r(string,PERMISSION_SEPERATOR,&tok);
	while(temp) {
		if(strcmp(temp,PERMISSION_READ_STRING)==0) {
			permission |= PERMISSION_READ;
		}
		else if(strcmp(temp,PERMISSION_ADD_STRING)==0) {
			permission |= PERMISSION_ADD;
		}
		else if(strcmp(temp,PERMISSION_CONTROL_STRING)==0) {
			permission |= PERMISSION_CONTROL;
		}
		else if(strcmp(temp,PERMISSION_ADMIN_STRING)==0) {
			permission |= PERMISSION_ADMIN;
		}
		else {
			ERROR("unknown permission \"%s\"\n",temp);
			exit(EXIT_FAILURE);
		}

		temp = strtok_r(NULL,PERMISSION_SEPERATOR,&tok);
	}

	return permission;
}

void initPermissions() {
	char * temp;
	char * cp2;
	char * password;
	unsigned int * permission;
        ConfigParam * param;

	permission_passwords = makeList(free, 1);

	permission_default = PERMISSION_READ | PERMISSION_ADD | 
				PERMISSION_CONTROL | PERMISSION_ADMIN;

        param = getNextConfigParam(CONF_PASSWORD, NULL);

        if(param) {
	        permission_default = 0;

                do {
		        if(!strstr(param->value, PERMISSION_PASSWORD_CHAR)) {
			        ERROR("\"%s\" not found in password string "
                                        "\"%s\", line %i\n",
					PERMISSION_PASSWORD_CHAR,
					param->value,
                                        param->line);
			        exit(EXIT_FAILURE);
		        }

		        if(!(temp = strtok_r(param->value,
                                        PERMISSION_PASSWORD_CHAR,&cp2))) {
			        ERROR("something weird just happened in permission.c\n");
			        exit(EXIT_FAILURE);
		        }

                
		        password = temp;

		        permission = malloc(sizeof(unsigned int));
		        *permission = parsePermissions(strtok_r(NULL,"",&cp2));

		        insertInList(permission_passwords,password,permission);
                } while((param = getNextConfigParam(CONF_PASSWORD, param)));
	}

        param = getConfigParam(CONF_DEFAULT_PERMS);

	if(param) permission_default = parsePermissions(param->value);

	sortList(permission_passwords);
}

int getPermissionFromPassword(char * password, unsigned int * permission) {
	void * foundPermission;

	if(findInList(permission_passwords,password,&foundPermission)) {
		*permission = *((unsigned int *)foundPermission);
		return 0;
	}

	return -1;
}

void finishPermissions() {
	freeList(permission_passwords);
}

unsigned int getDefaultPermissions() {
	return permission_default;
}
