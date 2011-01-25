/* btype.c - (c) 2011 by Stefan `Sec` Zehl <sec@42.org>
 * 
 * Small program to generate keyboard input on android devices
 * Uses /dev/input/event* to simulate keypresses on the hardware keyboard
 * Tested and written on the HTC Vision a.k.a. Desire Z
 *
 * NOTE: Due to its design it can only create the characters you can
 * reach on your keyboard. Install a more capable keymap if you need
 * more characters.
 * 
 * "THE BEER-WARE LICENSE" (Revision 42b):
 * <sec@42.org> wrote this file. As long as you retain this notice you
 * can do whatever you want with this stuff. If we meet some day, and
 * you think this stuff is worth it, you can buy me a beer in return
 * -- Sec
 *
 */

#define NONUMS    /* vision and similar keyboards w/o numbers */
#define UTF8
#undef  TEST      /* define to compile on non-android, for testing stuff */
#undef  DEBUGGING /* dumps lots of info during runtime */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <termios.h>

/* That's a google-supplied define from ui/KeycodeLabels.h */
#define VISION_KEYPAD

/* next line needed to work around a bug in ui/KeycodeLabels.h */
typedef struct KeycodeLabel KeycodeLabel;

#ifndef TEST
#include <ui/KeycodeLabels.h>
#include <cutils/properties.h>
#include <linux/input.h>
#else
#include "KeycodeLabels.h"
//#define uint32_t int
//#define uint16_t short
#endif


/* structure of .kcm.bin file */
struct KeyRecord {
	uint32_t keyevent;
	uint16_t key_display;
	uint16_t key_nr;
	uint16_t key;
	uint16_t key_s;
	uint16_t key_a;
	uint16_t key_sa;
};

typedef struct KeyRecord kr_t;

uint32_t count; /* no of keymap entries. */
kr_t * map;  	/* keymap */

#ifdef DEBUGGING
char **strs; 	/* keylayout strings */
#endif

int fd;  	/* "input" filedesc. */
float boottime;
uint16_t shift,alt,del;

int dbg=0;	/* Global "debugging enabled" */

#define M_NONE	0
#define M_SHIFT	1
#define M_ALT	2

/* utf8 'decoding' macros */
#define UT2(a,b)   ( ((a&31)<<6)  + (b&63) )
#define UT3(a,b,c) ( ((a&15)<<12) + ((b&63)<<6) + (c&63) )

void sendev(uint16_t chr,uint32_t press){
#ifndef TEST
	uint32_t evtime_sec,evtime_usec;
	uint16_t one = 1;
	char event[16];
	float now;

	now=time(NULL)-boottime;
	evtime_sec=now;
	evtime_usec=(now-evtime_sec)*1e9;

	memcpy(event+ 0, &evtime_sec,  4);
	memcpy(event+ 4, &evtime_usec, 4);
	memcpy(event+ 8, &one,         2); /* type == Keyboard event */
	memcpy(event+10, &chr,         2);
	memcpy(event+12, &press,       4);

	write(fd,event,16);
#else
	printf(">> %d %d\n",chr,press);
#endif
}

void sendcode(int mod, int chr){
	if(mod & M_SHIFT)
		sendev(shift, 1);
	if(mod & M_ALT)
		sendev(alt, 1);

	sendev(chr,1);
	sendev(chr,0);

	if(mod & M_ALT)
		sendev(alt, 0);
	if(mod & M_SHIFT)
		sendev(shift, 0);
}

void cache_keymap(char * path){
	FILE * keymap;
	char blob[16]; /* dummy store for header reads */

	keymap=fopen(path,"r");
	if (!keymap){
		perror("keymap open failed");
	}

	fread(blob,8,1,keymap);
	blob[8]=0;
	if(strcmp(blob,"keychar")!=0){
		fprintf(stderr,"Not a keymap?\n");
		exit (-1);
	};

	fread(blob,4,1,keymap);		/* 0x12345678, endianness marker */
	fread(blob,4,1,keymap);		/* 0x2, version  */
	fread(&count,4,1,keymap);	/* key count     */
	fread(blob,4,1,keymap);		/* keyboard type */
	fread(blob,8,1,keymap);		/* "Padding"     */

	if (dbg) printf("Keys: %d\n",count);

	map=(kr_t *)malloc(16*count);

	if(map==NULL){
		fprintf(stderr,"malloc() failed\n");
		exit(-1);
	}

	fread(map,count,16,keymap);
	fclose(keymap);

#ifdef DEBUGGING
	/* allocate space for the keylayout strings */
	strs=(char**)calloc(sizeof(char*),count);
	if(strs==NULL){
		fprintf(stderr,"malloc() failed\n");
		exit(-1);
	}
#endif
}

#define MAX_KEY_LINE_LEN 200
void cache_keylayout(char * path){
	FILE * keyl;
	char line[MAX_KEY_LINE_LEN];
	char str[MAX_KEY_LINE_LEN];
	int chr,x,dwim;

	keyl=fopen(path,"r");

	if (!keyl){
		perror("keylayout open failed");
	}

	for(x=0;x<count;x++) // we're misusing this field and need to clear it.
			map[x].key_display=0;


	while(!feof(keyl)){
		fgets(line,MAX_KEY_LINE_LEN,keyl);
		if(line[0]=='#')
			continue;
		if(line[0]=='\n')
			continue;

		if(sscanf(line,"key %d %s",&chr,str)!=2){ /* str is big enough! */
			fprintf(stderr,"failure to read line %s\n",line);
			exit(-1);
		};

#ifdef DEBUGGING
		printf ("read: %d, %s.\n",chr,str);
#endif

		/* Cache shift and alt key, we will need it later */
		if(strcmp(str,"SHIFT_LEFT")==0){
			shift=chr;
		};
		if(strcmp(str,"ALT_LEFT")==0){
			alt=chr;
		};
		if(strcmp(str,"DEL")==0){
			del=chr;
		};

		/* First: map the string to the keycode value */
		dwim=0;
		for(x=0;KEYCODES[x].literal != NULL ;x++){
			if(strcmp(KEYCODES[x].literal,str)==0){
				dwim=KEYCODES[x].value;
				break;
			};
		};

		if(dwim>0){
			/* If we found it, search for it in the keymap, 
			 * to see how we can generate it                */
			for(x=0;x<count;x++){
				if (dwim==map[x].keyevent){
					if(dbg)printf("mapped key %s (%d): %d\n",str,x,dwim);
#ifdef DEBUGGING
					strs[x]=(char*)malloc(strlen(str)+1);
					strcpy(strs[x],str);
#endif
					map[x].key_display=chr; /* misuse key_display to cache it */
					break;
				};
			};
		};
	};
	fclose(keyl);
}

void sendkey(int c){
	int mod=M_NONE;
	int r=0;
	int x;

	/* search for the key in our keymap */
	for(x=0;x<count;x++){
#ifdef NONUMS
		if (map[x].key>='0' && map[x].key<='9')
			continue;
#endif
		if (map[x].key_display==0) // skip nonexisting keys
			continue;

		if (c==map[x].key){
			mod=M_NONE; r=map[x].key_display; break;
		};
		if (c==map[x].key_s){
			mod=M_SHIFT; r=map[x].key_display; break;
		};
		if (c==map[x].key_a){
			mod=M_ALT; r=map[x].key_display; break;
		};
		if (c==map[x].key_sa){
			mod=M_ALT+M_SHIFT; r=map[x].key_display; break;
		};
	};
	if (r==0){
		fprintf(stderr,"\nsendkey: unknown key '%c' (%d)\n",c,c);
	}else{
		if(dbg)printf("sendkey: '%c' is a mod=%d scan=%d\n",c,mod,r);
	};
	sendcode(mod,r);
}

#ifdef TEST
#define INPUTDIR "./input"
#else
#define INPUTDIR "/dev/input"
#endif
void find_input(char *keyboard){
	DIR * inp;
	struct dirent * dir;
	char n[200];
	int found=0;


	inp=opendir(INPUTDIR);
	if(inp == NULL){
		fprintf(stderr,"Can't open " INPUTDIR "\n");
		exit(-1);
	};
	while( (dir=readdir(inp)) != NULL){
		if(dir->d_name[0]=='.')
			continue;

		snprintf(n,200,"%s/%s",INPUTDIR,dir->d_name);

		if(dbg)printf("Checking input: %s\n",n);

		if ((fd = open(n, O_RDWR)) < 0) {
			fprintf(stderr, "open('%s'): %s\n", n, strerror(errno));
			exit(-1);
		}

#ifdef TEST
		strcpy(n,keyboard);
#else
		if (ioctl(fd, EVIOCGNAME(200), n) < 0) {
			fprintf(stderr, "EVIOCGNAME: %s\n", strerror(errno));
			exit(-1);
		}
#endif
		if(dbg)printf("- it's a: %s\n",n);
		if(strcmp(n,keyboard)==0){
			found=1;
			break;
		};
		close(fd);
	};
	if(!found){
		fprintf(stderr,"Can't find input descriptor?\n");
		exit(-1);
	};
}

void get_boot_time(){
	FILE * boot;
	char line[MAX_KEY_LINE_LEN];
	float uptime;

	boot=fopen("/proc/uptime","r");
	if (!boot){
		perror("uptime open failed");
	}
	fgets(line,MAX_KEY_LINE_LEN,boot);
	uptime=atof(line);
	if(dbg)printf("Got uptime: %f\n",uptime);
	boottime=time((time_t*)NULL)-uptime;


	fclose(boot);
}

void interactive(FILE * f){
	struct termios tio;
	int c,c2,c3;

	/* enable sinle char input */
	tcgetattr(STDIN_FILENO,&tio);
	tio.c_lflag &=~ICANON;
	tcsetattr(STDIN_FILENO,TCSANOW,&tio);

	while( (c=getc(stdin)) != -1){

#ifdef UTF8
		/* will "eat keystrokes" on non-utf8 */
		if((c&(128+64+32))==(128+64)){
			c2=getc(stdin);
			c=UT2(c,c2);
		}else if( (c&(128+64+32+16))==(128+64+32)){
			c2=getc(stdin);
			c3=getc(stdin);
			c=UT3(c,c2,c3);
		};
#endif

		if(c==0x08 || c==0x7f){ /* delete/backspace */
			sendcode(0,del);
			continue;
		};

		sendkey(c);
	};
}


int main(int argc, char *argv[]) {
	char path[200];
	char *keyboard;
	int x;
	int idx=1;
	int btdt=0;
	unsigned int c;

	if (argc >1){
		if(strcmp(argv[idx],"-d")==0){
			idx++;
			dbg=1;
		};
	};

	/* get current keyboard name */
#ifndef TEST
	char value[PROPERTY_VALUE_MAX];
	property_get("hw.keyboards.0.devname", value, "");
	keyboard=value;
#else
	keyboard="vision-keypad-ger";
#endif


	/* get keymap file */
#ifdef TEST
	snprintf(path,200,"./%s.kcm.bin",keyboard);
#else
	snprintf(path,200,"/system/usr/keychars/%s.kcm.bin",keyboard);
#endif
	cache_keymap(path); /* sets (global) map */

	/* get keylayout file */
#ifdef TEST
	snprintf(path,200,"./%s.kl",keyboard);
#else
	snprintf(path,200,"/system/usr/keylayout/%s.kl",keyboard);
#endif
	cache_keylayout(path);

#ifdef DEBUGGING
	for(x=0;x<count;x++){
		printf ("Key %d: ev=%d nr=%d\n",x,map[x].keyevent,map[x].key_nr);
		printf ("- normal: %d\n",map[x].key);
		printf ("- shift:  %d\n",map[x].key_s);
		printf ("- alt:    %d\n",map[x].key_a);
		printf ("- s+a:    %d\n",map[x].key_sa);
		if(strs[x]!=NULL)
			printf ("- label: %s (%d)\n",strs[x],map[x].key_display);
		printf ("\n");
	};
	printf("shift=%d alt=%d\n",shift,alt);
#endif

	/* find input device */
	find_input(keyboard);

	get_boot_time();

	while (argc >idx){
		if(btdt)
			sendkey(' ');

		for (x=0;x<strlen(argv[idx]);x++){
			c=((unsigned char **)argv)[idx][x];

#ifdef UTF8
			/* cheap utf-8 parser hack */
			/* -  can't fall off end of string because of \0 */
			if((c&(128+64+32))==(128+64) &&
				 (argv[idx][x+1]&(128+64))==(128)){
 				c=UT2(c,argv[idx][x+1]);
				x++;
			}else if( (c&(128+64+32+16))==(128+64+32) &&
				 (argv[idx][x+1]&(128+64))==(128) &&
				 (argv[idx][x+2]&(128+64))==(128)){
 				c=UT3(c,argv[idx][x+1],argv[idx][x+2]);
				x+=2;
			};
#endif

			sendkey(c);
		};
		idx++;
		btdt=1;
	};

	/* nothing done yet? Enable life input */
	if(!btdt){
		interactive(stdin);
	};
	
	return 0;
}
