#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "winpe.h"
#define MS __attribute__((ms_abi))
#define W 64
#define H 80
#pragma pack(push,1)
struct gimg {
  void *data; uint16_t f08, width, height; uint8_t bits, f0f;
  uint8_t pad10[4]; uint32_t frames; uint16_t chan; uint8_t pad1a[14];
  uint8_t sensor28, sensor29; uint8_t pad2a[22];  // +0x28,+0x29 sensor_type
};
#pragma pack(pop)
typedef int MS (*ppp_t)(int);
typedef void *MS (*estart_t)(int *);
typedef int MS (*eadd_t)(void*,struct gimg*,void*,void*,uint8_t,void*);
static uint8_t px[W*H];
int main(void){
  winpe_set_verbose(0);
  if(!winpe_load("../../win-driver/AlgoMilan.dll")) return 1;
  ppp_t ppp=winpe_getproc("ppp_param_init");
  estart_t estart=winpe_getproc("enrolStartEx");
  eadd_t eadd=winpe_getproc("enrolAddImage");
  FILE*f=fopen("frames.bin","rb"); fseek(f,25L*W*H,SEEK_SET);
  if(fread(px,1,W*H,f)!=W*H){perror("r");return 1;} fclose(f);
  for(int pc=0; pc<=3; pc++){
    for(int st=0; st<=8; st++){
      ppp(pc);
      int maxt=16; void*sess=estart(&maxt); if(!sess){printf("pc=%d st=%d no sess\n",pc,st);continue;}
      struct gimg img; memset(&img,0,sizeof img);
      img.data=px; img.f08=H; img.width=W; img.height=H; img.bits=8; img.f0f=1;
      img.frames=1; img.chan=1; img.sensor28=st; img.sensor29=st;
      int o3=0,o4=0,o6=0;
      int r=eadd(sess,&img,&o3,&o4,0,&o6);
      printf("ppp=%d sensor_type=%d -> enrolAddImage=%d (0x%x)\n", pc, st, r, r);
    }
  }
  return 0;
}
