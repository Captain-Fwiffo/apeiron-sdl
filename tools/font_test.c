#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/font.h"
int main(void){ static uint32_t im[600*60]; for(int i=0;i<600*60;i++) im[i]=0xFF000000;
 char a[100],b[100]; int n=0; for(int c=32;c<127;c++) a[n++]=c; a[n]=0;
 char l1[60]; memcpy(l1,a,47); l1[47]=0; font_draw(im,600,60,4,4,2,0xFFFFFFFF,l1);
 char l2[60]; memcpy(l2,a+47,48); l2[48]=0; font_draw(im,600,60,4,30,2,0xFFFFFF00,l2);
 FILE*f=fopen("font.ppm","wb"); fprintf(f,"P6\n600 60\n255\n"); for(int i=0;i<600*60;i++){fputc(im[i]>>16&255,f);fputc(im[i]>>8&255,f);fputc(im[i]&255,f);} fclose(f); return 0; }
