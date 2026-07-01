/*
 * 
 *     DUNGEON OF DOOM  —  OpenGL + GLFW Edition         
 *                  Linguagem C                           
 *
 *  COMPILAR:
 *  ─────────────────────────────────────────────────────────────
 *  Requer miniaudio.h (header-only, sem custo):
 *    wget https://raw.githubusercontent.com/mackron/miniaudio/master/miniaudio.h
 *    
 *
 *
 *  Windows (MinGW):
 *    gcc dungeon_gl.c -o dungeon.exe -lglfw3 -lopengl32 -lgdi32 -lm -lwinmm -lole32
 *      -I"C:/glfw/include" -L"C:/glfw/lib"
 *    (baixar GLFW em https://www.glfw.org/download.html)
 *
 *  CONTROLES:
 *  ─────────────────────────────────────────────────────────────
 *  WASD / Setas  → Mover / Atacar adjacente
 *  ESPAÇO        → Atacar inimigo mais próximo
 *  1-5           → Usar item do inventário
 *  ENTER         → Confirmar (menu)
 *  R             → Reiniciar (após morte/vitória)
 *  ESC           → Sair
 */

#include <GL/gl.h>
#include <GLFW/glfw3.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* ═══════════════════════════════════════════════════════════════
   MINIAUDIO  —  síntese de áudio procedural 
/
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

/* protótipo necessário (definição completa mais abaixo, junto às utilidades) */
static float Clamp01(float v);

/* ── Tipos de som ─────────────────────────────────────────────
   SFX_SWING   : golpe do jogador (espada cortando o ar)
   SFX_HIT     : inimigo recebe dano (impacto grave)
   SFX_KILL    : inimigo morto (acorde descendente)
   SFX_HURT    : jogador recebe dano (ruído agudo)
   SFX_PICKUP  : pegar item (sino cintilante)
   SFX_POTION  : usar poção (glug borbulhante)
   SFX_STAIRS  : descer escada (whoosh + tom de confirmação)
   SFX_TRAP    : armadilha (clique metálico + alarme)
   SFX_LEVELUP : subir de nível (fanfarra ascendente)
   SFX_DEATH   : morte do jogador (som de queda dramático)
   SFX_WIN     : vitória (fanfarra épica)
   SFX_COUNT   : sentinela
─────────────────────────────────────────────────────────────── */
typedef enum {
    SFX_SWING=0, SFX_HIT, SFX_KILL, SFX_HURT,
    SFX_PICKUP, SFX_POTION, SFX_STAIRS, SFX_TRAP,
    SFX_LEVELUP, SFX_DEATH, SFX_WIN,
    SFX_COUNT
} SfxId;

#define AUDIO_RATE   44100
#define AUDIO_CH     1
#define SFX_VOICES   8          /* máximo de sons simultâneos */
#define MUSIC_BPM    72         /* batidas por minuto da trilha */

/* ─── Voice: uma voz de SFX em reprodução ─────────────────── */
typedef struct {
    float *buf;     /* amostras PCM float */
    int    len;     /* total de amostras */
    int    pos;     /* posição atual */
    float  vol;     /* volume 0-1 */
} Voice;

/* ─── Globais de áudio ────────────────────────────────────── */
static ma_device   gAudioDevice;
static float      *gSfxBuf[SFX_COUNT];  /* buffers pré-gerados */
static int         gSfxLen[SFX_COUNT];
static Voice       gVoices[SFX_VOICES];
static float      *gMusicBuf=NULL;       /* loop de música */
static int         gMusicLen=0;
static int         gMusicPos=0;
static float       gMusicVol=0.38f;
static float       gSfxVol=0.72f;
static int         gAudioOk=0;

/* ─── Callback de áudio (thread de áudio) ────────────────── */
static void AudioCB(ma_device *dev,void *out,const void *in,ma_uint32 frames){
    (void)dev;(void)in;
    float *dst=(float*)out;
    for(ma_uint32 i=0;i<frames;i++){
        float s=0.0f;
        /* música de fundo */
        if(gMusicBuf && gMusicLen>0){
            s += gMusicBuf[gMusicPos % gMusicLen] * gMusicVol;
            gMusicPos++;
        }
        /* vozes de sfx */
        for(int v=0;v<SFX_VOICES;v++){
            if(gVoices[v].buf && gVoices[v].pos < gVoices[v].len){
                s += gVoices[v].buf[gVoices[v].pos++] * gVoices[v].vol * gSfxVol;
            }
        }
        /* clamp */
        if(s> 1.0f) s= 1.0f;
        if(s<-1.0f) s=-1.0f;
        dst[i]=s;
    }
}

/* ─── Dispara um SFX (thread-safe para chamadas do jogo) ──── */
static void PlaySfx(SfxId id){
    if(!gAudioOk||id<0||id>=SFX_COUNT) return;
    /* procura voz livre ou reutiliza a mais adiantada */
    int slot=0; int best=0;
    for(int v=0;v<SFX_VOICES;v++){
        if(!gVoices[v].buf || gVoices[v].pos>=gVoices[v].len){ slot=v; break; }
        if(gVoices[v].pos > best){ best=gVoices[v].pos; slot=v; }
    }
    gVoices[slot].buf = gSfxBuf[id];
    gVoices[slot].len = gSfxLen[id];
    gVoices[slot].pos = 0;
    gVoices[slot].vol = 1.0f;
}

/* ══════════════════════════════════════════════════════════════
   SÍNTESE PROCEDURAL DE SOM
   Todas as funções abaixo geram PCM float [-1,1] puro em C,
   sem arquivos externos.  Inspiradas nas técnicas clássicas de
   síntese FM e wavetable usadas em chips YM2612 (Mega Drive)
   e SID (Commodore 64).
══════════════════════════════════════════════════════════════ */

/* utilidade: aloca buffer e retorna ponteiro */
static float* AllocBuf(int samples){
    return (float*)calloc(samples, sizeof(float));
}

/* envelope ADSR simples, retorna amplitude 0-1 */
static float ADSR(int t, int a, int d, int s_len, float sl, int r){
    if(t<a)           return (float)t/(float)a;
    if(t<a+d)         return 1.0f - (1.0f-sl)*(float)(t-a)/(float)d;
    if(t<a+d+s_len)   return sl;
    int rt=t-(a+d+s_len);
    if(rt<r)          return sl*(1.0f-(float)rt/(float)r);
    return 0.0f;
}

/* osciladores */
static float OSaw(float phase){ return 2.0f*(phase-(float)(int)phase)-1.0f; }
static float OSqr(float phase, float pw){ return (phase-(float)(int)phase)<pw?1.0f:-1.0f; }
static float OTri(float phase){ float p=phase-(float)(int)phase; return p<0.5f?4*p-1:3-4*p; }
static float ONoise(void){ return (float)(rand()%32767)/16383.5f-1.0f; }

/* ── 0: SFX_SWING — espada cortando o ar ─────────────────── */
static void GenSwing(void){
    int n=AUDIO_RATE/6; /* ~166ms */
    float *b=AllocBuf(n); gSfxBuf[SFX_SWING]=b; gSfxLen[SFX_SWING]=n;
    float ph=0;
    for(int i=0;i<n;i++){
        float t=(float)i/(float)n;
        float freq=900.0f - 600.0f*t;  /* varredura descendente */
        float env=ADSR(i, 10,40,n-200,0.0f,150);
        float s=OSaw(ph)*0.6f + ONoise()*0.15f;
        b[i]=s*env*0.7f;
        ph+=freq/(float)AUDIO_RATE;
    }
}

/* ── 1: SFX_HIT — impacto grave no inimigo ───────────────── */
static void GenHit(void){
    int n=AUDIO_RATE/5;
    float *b=AllocBuf(n); gSfxBuf[SFX_HIT]=b; gSfxLen[SFX_HIT]=n;
    float ph=0;
    for(int i=0;i<n;i++){
        float env=ADSR(i,5,60,n-200,0.0f,140);
        float freq=180.0f-(float)i*0.15f;
        float s=OSqr(ph,0.4f)*0.7f + ONoise()*0.4f;
        b[i]=s*env*0.75f;
        ph+=freq/(float)AUDIO_RATE;
    }
}

/* ── 2: SFX_KILL — acorde descendente (inimigo morto) ────── */
static void GenKill(void){
    int n=AUDIO_RATE*3/8;
    float *b=AllocBuf(n); gSfxBuf[SFX_KILL]=b; gSfxLen[SFX_KILL]=n;
    float freqs[3]={440,330,220};
    float phs[3]={0,0,0};
    for(int i=0;i<n;i++){
        float env=ADSR(i,20,80,n/2,0.3f,n/4);
        float s=0;
        for(int h=0;h<3;h++){
            s+=OTri(phs[h]);
            phs[h]+=freqs[h]/(float)AUDIO_RATE;
            freqs[h]*=0.9999f; /* descida lenta */
        }
        b[i]=s/3.0f*env*0.65f;
    }
}

/* ── 3: SFX_HURT — jogador leva dano ─────────────────────── */
static void GenHurt(void){
    int n=AUDIO_RATE/4;
    float *b=AllocBuf(n); gSfxBuf[SFX_HURT]=b; gSfxLen[SFX_HURT]=n;
    float ph=0;
    for(int i=0;i<n;i++){
        float env=ADSR(i,3,30,n/2,0.0f,n/3);
        float freq=600.0f+ONoise()*80.0f;
        float s=ONoise()*0.5f + OSqr(ph,0.5f)*0.5f;
        b[i]=s*env*0.8f;
        ph+=freq/(float)AUDIO_RATE;
    }
}

/* ── 4: SFX_PICKUP — pegar item (sino brilhante) ─────────── */
static void GenPickup(void){
    int n=AUDIO_RATE/4;
    float *b=AllocBuf(n); gSfxBuf[SFX_PICKUP]=b; gSfxLen[SFX_PICKUP]=n;
    float phs[4]={0,0,0,0};
    float fs[4]={880,1108,1320,1760};
    for(int i=0;i<n;i++){
        float env=ADSR(i,5,20,n/3,0.4f,n/2);
        float s=0;
        for(int h=0;h<4;h++){ s+=sinf(phs[h]*2*3.14159f); phs[h]+=fs[h]/(float)AUDIO_RATE; }
        b[i]=s/4.0f*env*0.55f;
    }
}

/* ── 5: SFX_POTION — glug borbulhante ────────────────────── */
static void GenPotion(void){
    int n=AUDIO_RATE*2/5;
    float *b=AllocBuf(n); gSfxBuf[SFX_POTION]=b; gSfxLen[SFX_POTION]=n;
    float ph=0;
    for(int i=0;i<n;i++){
        /* dois "glugs" modulados */
        float t=(float)i/(float)AUDIO_RATE;
        float freq=320.0f+sinf(t*28.0f)*120.0f;
        float env=ADSR(i,8,20,n-200,0.0f,180);
        float bub=(i%(AUDIO_RATE/8)<AUDIO_RATE/20)?1.0f:0.3f;
        b[i]=(OTri(ph)*0.5f+ONoise()*0.2f)*env*bub*0.6f;
        ph+=freq/(float)AUDIO_RATE;
    }
}

/* ── 6: SFX_STAIRS — descida de escada ───────────────────── */
static void GenStairs(void){
    int n=AUDIO_RATE/2;
    float *b=AllocBuf(n); gSfxBuf[SFX_STAIRS]=b; gSfxLen[SFX_STAIRS]=n;
    /* whoosh + arpejo pentatônico ascendente */
    float notes[5]={261,329,392,523,659};
    int steps=5, stepLen=n/8;
    float ph=0,wph=0;
    for(int i=0;i<n;i++){
        float env=ADSR(i,30,60,n/2,0.3f,n/4);
        /* whoosh */
        float whoosh=ONoise()*(1.0f-(float)i/(float)n)*0.3f;
        /* arpejo */
        float arp=0;
        if(i<steps*stepLen){
            int step=i/stepLen;
            float f=notes[step];
            float ae=ADSR(i-step*stepLen,5,10,stepLen-30,0.0f,20);
            arp=sinf(ph*2*3.14159f)*ae;
            ph+=f/(float)AUDIO_RATE;
        }
        b[i]=(whoosh+arp*0.5f)*env*0.65f;
        wph+=(float)AUDIO_RATE/(float)AUDIO_RATE;
    }
}

/* ── 7: SFX_TRAP — clique metálico + alarme ──────────────── */
static void GenTrap(void){
    int n=AUDIO_RATE/3;
    float *b=AllocBuf(n); gSfxBuf[SFX_TRAP]=b; gSfxLen[SFX_TRAP]=n;
    float ph=0;
    for(int i=0;i<n;i++){
        float env=ADSR(i,2,20,n/3,0.4f,n/3);
        float freq=(i<n/5)?900.0f:400.0f+sinf((float)i*0.05f)*150.0f;
        float s=(OSqr(ph,0.5f)*0.6f+ONoise()*0.35f)*env;
        b[i]=s*0.75f;
        ph+=freq/(float)AUDIO_RATE;
    }
}

/* ── 8: SFX_LEVELUP — fanfarra ascendente ────────────────── */
static void GenLevelUp(void){
    int n=AUDIO_RATE*3/4;
    float *b=AllocBuf(n); gSfxBuf[SFX_LEVELUP]=b; gSfxLen[SFX_LEVELUP]=n;
    /* acorde maior + arpejo C-E-G-C5 rápido */
    float chord[4]={261,329,392,523};
    float phs[4]={0,0,0,0};
    for(int i=0;i<n;i++){
        float env=ADSR(i,15,40,n/2,0.5f,n/3);
        float s=0;
        /* intro: arpejo rápido nos primeiros 200ms */
        int onset=i*4/n;
        for(int h=0;h<=onset&&h<4;h++){
            s+=OTri(phs[h])+sinf(phs[h]*2*3.14159f)*0.5f;
            phs[h]+=chord[h]/(float)AUDIO_RATE;
        }
        b[i]=s/(float)(onset+1)*env*0.45f;
    }
}

/* ── 9: SFX_DEATH — queda dramática ──────────────────────── */
static void GenDeath(void){
    int n=AUDIO_RATE;
    float *b=AllocBuf(n); gSfxBuf[SFX_DEATH]=b; gSfxLen[SFX_DEATH]=n;
    float ph=0;
    for(int i=0;i<n;i++){
        float t=(float)i/(float)n;
        float freq=220.0f*(1.0f-t*0.7f);  /* pitch bend para baixo */
        float env=ADSR(i,5,80,n/2,0.3f,n/3);
        float s=OSaw(ph)*0.5f + ONoise()*(t*0.4f);
        b[i]=s*env*0.7f;
        ph+=freq/(float)AUDIO_RATE;
    }
}

/* ── 10: SFX_WIN — fanfarra épica ────────────────────────── */
static void GenWin(void){
    int n=AUDIO_RATE*2;
    float *b=AllocBuf(n); gSfxBuf[SFX_WIN]=b; gSfxLen[SFX_WIN]=n;
    /* Progressão C-F-G-C em onda quadrada */
    float prog[4]={261,349,392,523};
    int segLen=n/4;
    float ph1=0,ph2=0,ph3=0;
    for(int i=0;i<n;i++){
        int seg=i/segLen; if(seg>=4) seg=3;
        float f=prog[seg];
        float env=ADSR(i%segLen, 10,30,segLen-100,0.6f,60);
        float genv=ADSR(i,20,100,n-500,0.7f,400);
        float s=(OSqr(ph1,0.5f)+OTri(ph2)*0.5f+sinf(ph3*2*3.14159f)*0.3f)/1.8f;
        b[i]=s*env*genv*0.55f;
        ph1+=f/(float)AUDIO_RATE;
        ph2+=f*1.5f/(float)AUDIO_RATE;
        ph3+=f*2.0f/(float)AUDIO_RATE;
    }
}

/* ══════════════════════════════════════════════════════════════
   MÚSICA DE FUNDO — Trilha procedural estilo dungeon crawler
   ─────────────────────────────────────────────────────────────
   Estrutura: loop de 8 compassos em Dó menor, 72 BPM
   Vozes: baixo pulsante (raiz do acorde), melodia FM,
          pad de cordas suave, hi-hat de percussão.

   Inspiração rítmica: música de dungeon crawler dos anos 90
   (Ultima Underworld, Eye of the Beholder).
   Síntese FM simples: operador modulador + carreador, sem
   arquivo de patches — todos os parâmetros são hard-coded.
══════════════════════════════════════════════════════════════ */
static void GenMusic(void){
    /* 8 compassos de 4/4 a 72 BPM */
    float bps=MUSIC_BPM/60.0f;
    float beatSec=1.0f/bps;
    int beatSamp=(int)(beatSec*(float)AUDIO_RATE);
    int bars=8, beatsPerBar=4;
    int totalSamp=bars*beatsPerBar*beatSamp;

    float *b=AllocBuf(totalSamp); gMusicBuf=b; gMusicLen=totalSamp;

    /* escala de Dó menor natural: C D Eb F G Ab Bb C */
    float scl[8]={130.81f,146.83f,155.56f,174.61f,196.00f,207.65f,233.08f,261.63f};

    /* linha de baixo: notas por compasso (índice na escala) */
    int bassLine[8]={0,4,3,4, 0,5,6,4};

    /* melodia: 16 notas (uma a cada meia batida) */
    int melNotes[16]={7,5,4,3, 4,5,7,6, 5,4,3,2, 3,4,5,0};

    float bassph=0, melph=0, melmod=0;
    float padph[3]={0,0,0};

    for(int i=0;i<totalSamp;i++){
        float t=(float)i/(float)AUDIO_RATE;
        int beat=(int)(t/beatSec);
        int bar=beat/(beatsPerBar);
        if(bar>=bars) bar=bars-1;
        int beatInBar=beat%beatsPerBar;
        float beatFrac=(t-beat*beatSec)/beatSec;

        /* ── BAIXO: onda quadrada com envelope de nota ──── */
        int bassIdx=bassLine[bar%8];
        float bassFreq=scl[bassIdx];
        float bassEnv=(beatFrac<0.6f)?
            ADSR((int)(beatFrac*beatSamp),5,30,beatSamp/2,0.5f,beatSamp/5):0.0f;
        float bass=OSqr(bassph,0.5f)*bassEnv*0.28f;
        bassph+=bassFreq/(float)AUDIO_RATE;

        /* ── MELODIA FM: carreador + modulador ──────────── */
        int melIdx=((beat*2+(int)(beatFrac*2)))%16;
        float melFreq=scl[melNotes[melIdx%16]]*2.0f;
        float melMod=sinf(melmod*2*3.14159f)*melFreq*1.2f;
        float melEnv=ADSR((int)(beatFrac*beatSamp/2),3,20,beatSamp/4,0.4f,beatSamp/6);
        float mel=sinf((melph*2*3.14159f)+melMod)*melEnv*0.18f;
        melmod+=melFreq*2.1f/(float)AUDIO_RATE;
        melph+=melFreq/(float)AUDIO_RATE;

        /* ── PAD DE CORDAS: três osciladores detunados ─── */
        float padFreq=scl[bassIdx]*4.0f;
        float padEnv=0.12f;
        float pad=(sinf(padph[0]*2*3.14159f)+
                   sinf(padph[1]*2*3.14159f)+
                   sinf(padph[2]*2*3.14159f))/3.0f*padEnv;
        padph[0]+= padFreq*0.998f/(float)AUDIO_RATE;
        padph[1]+= padFreq/(float)AUDIO_RATE;
        padph[2]+= padFreq*1.002f/(float)AUDIO_RATE;

        /* ── PERCUSSÃO: kick no 1 e 3, hi-hat nos 2 e 4 ── */
        float perc=0;
        if(beatFrac<0.08f){
            if(beatInBar==0||beatInBar==2){
                /* kick: ruído + tom descendente */
                float kf=120.0f*(1.0f-beatFrac*12.0f);
                perc=ONoise()*0.1f + OSqr((float)i*kf/(float)AUDIO_RATE,0.5f)*
                    (1.0f-beatFrac*12.0f)*0.22f;
            } else {
                /* hi-hat: ruído com envelope rápido */
                perc=ONoise()*(1.0f-beatFrac*18.0f)*0.08f;
            }
        }

        b[i]=Clamp01(bass+mel+pad+perc)*0.85f;
        /* pequena distorção suave */
        b[i]=tanhf(b[i]*1.4f)*0.72f;
    }
}

/* ─── Inicializa o sistema de áudio ──────────────────────── */
static void AudioInit(void){
    /* gera todos os SFX */
    GenSwing(); GenHit();  GenKill(); GenHurt();
    GenPickup(); GenPotion(); GenStairs(); GenTrap();
    GenLevelUp(); GenDeath(); GenWin();
    GenMusic();

    ma_device_config cfg=ma_device_config_init(ma_device_type_playback);
    cfg.playback.format  =ma_format_f32;
    cfg.playback.channels=AUDIO_CH;
    cfg.sampleRate       =AUDIO_RATE;
    cfg.dataCallback     =AudioCB;

    if(ma_device_init(NULL,&cfg,&gAudioDevice)!=MA_SUCCESS){
        fprintf(stderr,"[audio] falha ao iniciar dispositivo\n");
        return;
    }
    if(ma_device_start(&gAudioDevice)!=MA_SUCCESS){
        fprintf(stderr,"[audio] falha ao iniciar playback\n");
        ma_device_uninit(&gAudioDevice);
        return;
    }
    gAudioOk=1;
}

static void AudioShutdown(void){
    if(gAudioOk){
        ma_device_uninit(&gAudioDevice);
        for(int i=0;i<SFX_COUNT;i++) free(gSfxBuf[i]);
        free(gMusicBuf);
    }
}

/* ═══════════════════════════════════════════════════════════════
   CONSTANTES
═══════════════════════════════════════════════════════════════ */
#define SW          960
#define SH          700
#define TILE        28
#define MAP_W       28
#define MAP_H       20
#define MAX_ENEMIES 22
#define MAX_ITEMS   16
#define MAX_INV      5
#define MAX_ROOMS   10
#define MAX_PART    120
#define MAX_LOG      7
#define FOV_R        8
#define MAX_RANK     10   /* top N exibido/salvo */
#define NAME_MAXLEN  15

/* ═══════════════════════════════════════════════════════════════
   TIPOS
═══════════════════════════════════════════════════════════════ */
typedef enum { TILE_WALL, TILE_FLOOR, TILE_STAIRS, TILE_TRAP } TileType;
typedef enum { E_GOBLIN, E_ORC, E_TROLL, E_VAMPIRE, E_DRAGON  } EnemyType;
typedef enum { I_POTION, I_SWORD, I_SHIELD, I_ARMOR,
               I_FIRE,   I_ICE,   I_BOOTS              } ItemType;
typedef enum { GS_MENU, GS_NAME, GS_PLAY, GS_DEAD, GS_WIN, GS_RANKING } GameState;

typedef struct { float r,g,b,a; } Col4;
typedef struct { float x,y,vx,vy,life,maxLife; Col4 col; int sz; } Particle;

typedef struct {
    TileType type;
    int visible, explored;
} Tile;

typedef struct {
    int x,y,w,h;
} Room;

typedef struct {
    ItemType type;
    int mx, my;     /* posição no mapa; -1 = no inventário */
    int active;
    const char *name;
    int value;
} Item;

typedef struct {
    EnemyType type;
    int x,y,hp,maxHp,atk,def,exp,alive,alerted;
    int moveTimer, moveDelay, atkDelay;
    int frozenT; /* turnos restantes congelado pelo pergaminho de gelo */
    const char *name;
    Col4 col;
} Enemy;

typedef struct {
    int x,y,hp,maxHp,atk,def,level,exp,expNext,floor;
    int atkBonus,defBonus,invCount;
    int frozen,frozenT;
    Item inv[MAX_INV];
    int hasShield,hasBoots;
} Player;

typedef struct {
    char name[NAME_MAXLEN+1];
    int  score;
    int  level;
    int  floor;
    int  turns;
    int  won; /* 1 = venceu o jogo, 0 = morreu */
} RankEntry;

/* ═══════════════════════════════════════════════════════════════
   GLOBAIS
═══════════════════════════════════════════════════════════════ */
static GLFWwindow *win;
static Tile        map[MAP_H][MAP_W];
static Player      plr;
static Enemy       ens[MAX_ENEMIES];
static Item        its[MAX_ITEMS];
static Particle    pts[MAX_PART];
static Room        rooms[MAX_ROOMS];
static int         roomCnt=0;
static GameState   gs=GS_MENU;
static char        logMsg[MAX_LOG][160];
static int         logCnt=0;
static int         score=0,turn=0;
static float       shakeMag=0,shakeT=0;
static double      lastTime=0,dt=0;
static int         menuTick=0;
static int         keyBuf[16];   /* teclas recém pressionadas */
static int         keyBufN=0;

/* ─── Ranking persistente ──────────────────────────────────── */
#define RANK_FILE "ranking.txt"
static RankEntry   rankings[MAX_RANK];
static int         rankCnt=0;
static char        playerName[NAME_MAXLEN+1]="";
static int         nameLen=0;
static int         pendingRankInsert=0; /* 1 = ainda precisa registrar score ao final */

/* ═══════════════════════════════════════════════════════════════
   PROTÓTIPOS
═══════════════════════════════════════════════════════════════ */
void GenDungeon(void);
void PlaceEnemies(void);
void PlaceItems(void);
void UpdateFOV(void);
int  HasLOS(int x0,int y0,int x1,int y1);
void MovePlayer(int dx,int dy);
void PlrAttack(int ex,int ey);
void EnemyTurn(void);
void MoveEnemy(Enemy *e);
void EnemyAttack(Enemy *e);
void UseItem(int slot);
void TryPickup(void);
void AddLog(const char *m);
void SpawnParts(int tx,int ty,Col4 c,int n);
void UpdateParts(void);
void ResetGame(void);
int  EnemyAt(int x,int y);
int  IsWalkable(int x,int y);
void ConnectRooms(Room a,Room b);
void LevelUp(void);
void ScreenShake(float mag,float dur);

/* ranking */
void RankLoad(void);
void RankSave(void);
void RankInsert(const char *name,int score,int level,int floor,int turns,int won);
void DrawNameInput(void);
void DrawRanking(void);

/* draw helpers */
void DrawRect(float x,float y,float w,float h,Col4 c);
void DrawRectBorder(float x,float y,float w,float h,Col4 c,float bw,Col4 bc);
void DrawChar(float x,float y,float sz,char ch,Col4 c);
void DrawString(float x,float y,float sz,const char *s,Col4 c);
void DrawBar(float x,float y,float w,float h,float ratio,Col4 bg,Col4 fg);
void DrawCircleFill(float cx,float cy,float r,Col4 c,int segs);
void DrawTriangle(float x,float y,float sz,Col4 c);
void DrawDiamond(float x,float y,float sz,Col4 c);

/* scene draws */
void DrawMap(void);
void DrawHUD(void);
void DrawLogPanel(void);
void DrawParticles(void);
void DrawMenu(void);
void DrawDead(void);
void DrawWin(void);
void DrawFrame(void);

/* glfw callback */
void KeyCB(GLFWwindow*,int,int,int,int);
void CharCB(GLFWwindow*,unsigned int);

/* ═══════════════════════════════════════════════════════════════
   UTILIDADES
═══════════════════════════════════════════════════════════════ */
static int RR(int lo,int hi){ return lo + rand()%(hi-lo+1); }
static float Clamp01(float v){ return v<0?0:v>1?1:v; }
static Col4 C4(float r,float g,float b,float a){ Col4 c={r,g,b,a}; return c; }
static Col4 C4dim(Col4 c,float f){ return C4(c.r*f,c.g*f,c.b*f,c.a); }
static Col4 C4fade(Col4 c,float a){ return C4(c.r,c.g,c.b,a); }

/* ═══════════════════════════════════════════════════════════════
   OPENGL 2D — PRIMITIVAS
═══════════════════════════════════════════════════════════════ */
void DrawRect(float x,float y,float w,float h,Col4 c){
    glColor4f(c.r,c.g,c.b,c.a);
    glBegin(GL_QUADS);
      glVertex2f(x,y); glVertex2f(x+w,y);
      glVertex2f(x+w,y+h); glVertex2f(x,y+h);
    glEnd();
}

void DrawRectBorder(float x,float y,float w,float h,Col4 c,float bw,Col4 bc){
    DrawRect(x,y,w,h,c);
    glColor4f(bc.r,bc.g,bc.b,bc.a);
    glLineWidth(bw);
    glBegin(GL_LINE_LOOP);
      glVertex2f(x,y); glVertex2f(x+w,y);
      glVertex2f(x+w,y+h); glVertex2f(x,y+h);
    glEnd();
}

void DrawCircleFill(float cx,float cy,float r,Col4 c,int segs){
    glColor4f(c.r,c.g,c.b,c.a);
    glBegin(GL_TRIANGLE_FAN);
      glVertex2f(cx,cy);
      for(int i=0;i<=segs;i++){
          float a=(float)i/(float)segs*2.0f*3.14159265f;
          glVertex2f(cx+cosf(a)*r, cy+sinf(a)*r);
      }
    glEnd();
}

void DrawTriangle(float x,float y,float sz,Col4 c){
    glColor4f(c.r,c.g,c.b,c.a);
    glBegin(GL_TRIANGLES);
      glVertex2f(x+sz*0.5f, y);
      glVertex2f(x, y+sz);
      glVertex2f(x+sz, y+sz);
    glEnd();
}

void DrawDiamond(float x,float y,float sz,Col4 c){
    float hx=x+sz*0.5f, hy=y+sz*0.5f;
    glColor4f(c.r,c.g,c.b,c.a);
    glBegin(GL_QUADS);
      glVertex2f(hx, y);
      glVertex2f(x+sz, hy);
      glVertex2f(hx, y+sz);
      glVertex2f(x, hy);
    glEnd();
}

void DrawBar(float x,float y,float w,float h,float ratio,Col4 bg,Col4 fg){
    DrawRect(x,y,w,h,bg);
    if(ratio>0) DrawRect(x,y,w*Clamp01(ratio),h,fg);
}

/* ─── Fonte bitmap minimalista 5×7 ─────────────────────────────
   Cada caractere é codificado como 5 colunas de bits (7 linhas).
   Suporta A-Z, 0-9, espaço, e alguns símbolos.               */

static const unsigned char FONT[96][5]={
/*SP*/  {0x00,0x00,0x00,0x00,0x00},
/*!*/   {0x00,0x00,0x5F,0x00,0x00},
/*"*/   {0x00,0x07,0x00,0x07,0x00},
/*#*/   {0x14,0x7F,0x14,0x7F,0x14},
/*$*/   {0x24,0x2A,0x7F,0x2A,0x12},
/*%*/   {0x23,0x13,0x08,0x64,0x62},
/*&*/   {0x36,0x49,0x55,0x22,0x50},
/*'*/   {0x00,0x05,0x03,0x00,0x00},
/*(*/   {0x00,0x1C,0x22,0x41,0x00},
/*)*/   {0x00,0x41,0x22,0x1C,0x00},
/***/   {0x14,0x08,0x3E,0x08,0x14},
/*+*/   {0x08,0x08,0x3E,0x08,0x08},
/*,*/   {0x00,0x50,0x30,0x00,0x00},
/*-*/   {0x08,0x08,0x08,0x08,0x08},
/*.*/   {0x00,0x60,0x60,0x00,0x00},
/*/ */  {0x20,0x10,0x08,0x04,0x02},
/*0*/   {0x3E,0x51,0x49,0x45,0x3E},
/*1*/   {0x00,0x42,0x7F,0x40,0x00},
/*2*/   {0x42,0x61,0x51,0x49,0x46},
/*3*/   {0x21,0x41,0x45,0x4B,0x31},
/*4*/   {0x18,0x14,0x12,0x7F,0x10},
/*5*/   {0x27,0x45,0x45,0x45,0x39},
/*6*/   {0x3C,0x4A,0x49,0x49,0x30},
/*7*/   {0x01,0x71,0x09,0x05,0x03},
/*8*/   {0x36,0x49,0x49,0x49,0x36},
/*9*/   {0x06,0x49,0x49,0x29,0x1E},
/*:*/   {0x00,0x36,0x36,0x00,0x00},
/*;*/   {0x00,0x56,0x36,0x00,0x00},
/*<*/   {0x08,0x14,0x22,0x41,0x00},
/*=*/   {0x14,0x14,0x14,0x14,0x14},
/*>*/   {0x00,0x41,0x22,0x14,0x08},
/*?*/   {0x02,0x01,0x51,0x09,0x06},
/*@*/   {0x32,0x49,0x79,0x41,0x3E},
/*A*/   {0x7E,0x11,0x11,0x11,0x7E},
/*B*/   {0x7F,0x49,0x49,0x49,0x36},
/*C*/   {0x3E,0x41,0x41,0x41,0x22},
/*D*/   {0x7F,0x41,0x41,0x22,0x1C},
/*E*/   {0x7F,0x49,0x49,0x49,0x41},
/*F*/   {0x7F,0x09,0x09,0x09,0x01},
/*G*/   {0x3E,0x41,0x49,0x49,0x7A},
/*H*/   {0x7F,0x08,0x08,0x08,0x7F},
/*I*/   {0x00,0x41,0x7F,0x41,0x00},
/*J*/   {0x20,0x40,0x41,0x3F,0x01},
/*K*/   {0x7F,0x08,0x14,0x22,0x41},
/*L*/   {0x7F,0x40,0x40,0x40,0x40},
/*M*/   {0x7F,0x02,0x04,0x02,0x7F},
/*N*/   {0x7F,0x04,0x08,0x10,0x7F},
/*O*/   {0x3E,0x41,0x41,0x41,0x3E},
/*P*/   {0x7F,0x09,0x09,0x09,0x06},
/*Q*/   {0x3E,0x41,0x51,0x21,0x5E},
/*R*/   {0x7F,0x09,0x19,0x29,0x46},
/*S*/   {0x46,0x49,0x49,0x49,0x31},
/*T*/   {0x01,0x01,0x7F,0x01,0x01},
/*U*/   {0x3F,0x40,0x40,0x40,0x3F},
/*V*/   {0x1F,0x20,0x40,0x20,0x1F},
/*W*/   {0x3F,0x40,0x38,0x40,0x3F},
/*X*/   {0x63,0x14,0x08,0x14,0x63},
/*Y*/   {0x07,0x08,0x70,0x08,0x07},
/*Z*/   {0x61,0x51,0x49,0x45,0x43},
/*[*/   {0x00,0x7F,0x41,0x41,0x00},
/*\*/   {0x02,0x04,0x08,0x10,0x20},
/*]*/   {0x00,0x41,0x41,0x7F,0x00},
/*^*/   {0x04,0x02,0x01,0x02,0x04},
/*_*/   {0x40,0x40,0x40,0x40,0x40},
/*`*/   {0x00,0x01,0x02,0x04,0x00},
/*a*/   {0x20,0x54,0x54,0x54,0x78},
/*b*/   {0x7F,0x48,0x44,0x44,0x38},
/*c*/   {0x38,0x44,0x44,0x44,0x20},
/*d*/   {0x38,0x44,0x44,0x48,0x7F},
/*e*/   {0x38,0x54,0x54,0x54,0x18},
/*f*/   {0x08,0x7E,0x09,0x01,0x02},
/*g*/   {0x0C,0x52,0x52,0x52,0x3E},
/*h*/   {0x7F,0x08,0x04,0x04,0x78},
/*i*/   {0x00,0x44,0x7D,0x40,0x00},
/*j*/   {0x20,0x40,0x44,0x3D,0x00},
/*k*/   {0x7F,0x10,0x28,0x44,0x00},
/*l*/   {0x00,0x41,0x7F,0x40,0x00},
/*m*/   {0x7C,0x04,0x18,0x04,0x78},
/*n*/   {0x7C,0x08,0x04,0x04,0x78},
/*o*/   {0x38,0x44,0x44,0x44,0x38},
/*p*/   {0x7C,0x14,0x14,0x14,0x08},
/*q*/   {0x08,0x14,0x14,0x18,0x7C},
/*r*/   {0x7C,0x08,0x04,0x04,0x08},
/*s*/   {0x48,0x54,0x54,0x54,0x20},
/*t*/   {0x04,0x3F,0x44,0x40,0x20},
/*u*/   {0x3C,0x40,0x40,0x40,0x7C},
/*v*/   {0x1C,0x20,0x40,0x20,0x1C},
/*w*/   {0x3C,0x40,0x30,0x40,0x3C},
/*x*/   {0x44,0x28,0x10,0x28,0x44},
/*y*/   {0x0C,0x50,0x50,0x50,0x3C},
/*z*/   {0x44,0x64,0x54,0x4C,0x44},
/*{*/   {0x00,0x08,0x36,0x41,0x00},
/*|*/   {0x00,0x00,0x7F,0x00,0x00},
/*}*/   {0x00,0x41,0x36,0x08,0x00},
/*~*/   {0x10,0x08,0x08,0x10,0x08},
/*DEL*/ {0x00,0x00,0x00,0x00,0x00},
};

void DrawChar(float x,float y,float sz,char ch,Col4 c){
    int idx=(int)ch-32;
    if(idx<0||idx>=96) idx=0;
    const unsigned char *col=FONT[idx];
    float px=sz/5.0f, py=sz/7.0f;
    glColor4f(c.r,c.g,c.b,c.a);
    for(int cx=0;cx<5;cx++){
        unsigned char bits=col[cx];
        for(int row=0;row<7;row++){
            if(bits&(1<<row)){
                float rx=x+(float)cx*px;
                float ry=y+(float)row*py;
                glBegin(GL_QUADS);
                  glVertex2f(rx,ry);   glVertex2f(rx+px,ry);
                  glVertex2f(rx+px,ry+py); glVertex2f(rx,ry+py);
                glEnd();
            }
        }
    }
}

void DrawString(float x,float y,float sz,const char *s,Col4 c){
    float cx=x;
    while(*s){
        DrawChar(cx,y,sz,*s,c);
        cx+=sz*1.1f;
        s++;
    }
}

/* ═══════════════════════════════════════════════════════════════
   GERAÇÃO DO DUNGEON
═══════════════════════════════════════════════════════════════ */
void ConnectRooms(Room a,Room b){
    int ax=a.x+a.w/2, ay=a.y+a.h/2;
    int bx=b.x+b.w/2, by=b.y+b.h/2;
    int cx=ax,cy=ay;
    while(cx!=bx){ map[cy][cx].type=TILE_FLOOR; cx+=(bx>cx)?1:-1; }
    while(cy!=by){ map[cy][cx].type=TILE_FLOOR; cy+=(by>cy)?1:-1; }
}

void GenDungeon(void){
    /* limpa mapa */
    for(int y=0;y<MAP_H;y++)
        for(int x=0;x<MAP_W;x++){
            map[y][x].type=TILE_WALL;
            map[y][x].visible=map[y][x].explored=0;
        }

    roomCnt=0;
    int attempts=0;
    while(roomCnt<MAX_ROOMS && attempts<300){
        attempts++;
        Room r;
        r.w=RR(4,8); r.h=RR(3,6);
        r.x=RR(1,MAP_W-r.w-2);
        r.y=RR(1,MAP_H-r.h-2);

        int overlap=0;
        for(int i=0;i<roomCnt;i++){
            Room o=rooms[i];
            if(r.x<o.x+o.w+1&&r.x+r.w+1>o.x&&
               r.y<o.y+o.h+1&&r.y+r.h+1>o.y){overlap=1;break;}
        }
        if(overlap) continue;

        for(int ry=r.y;ry<r.y+r.h;ry++)
            for(int rx=r.x;rx<r.x+r.w;rx++)
                map[ry][rx].type=TILE_FLOOR;

        if(roomCnt>0) ConnectRooms(rooms[roomCnt-1],r);
        rooms[roomCnt++]=r;
    }

    /* jogador na primeira sala */
    Room f=rooms[0];
    plr.x=f.x+f.w/2; plr.y=f.y+f.h/2;

    /* escada na última sala */
    Room l=rooms[roomCnt-1];
    map[l.y+l.h/2][l.x+l.w/2].type=TILE_STAIRS;

    /* armadilhas */
    for(int i=2;i<roomCnt;i++){
        if(RR(0,2)==0){
            Room tr=rooms[i];
            int tx=tr.x+RR(1,tr.w-2);
            int ty=tr.y+RR(1,tr.h-2);
            if(map[ty][tx].type==TILE_FLOOR)
                map[ty][tx].type=TILE_TRAP;
        }
    }

    PlaceEnemies();
    PlaceItems();
    UpdateFOV();
}

/* ─── Inimigos ──────────────────────────────────────────────── */
static Col4 ECol(EnemyType t){
    switch(t){
        case E_GOBLIN:  return C4(0.3f,0.8f,0.3f,1);
        case E_ORC:     return C4(0.9f,0.5f,0.2f,1);
        case E_TROLL:   return C4(0.4f,0.4f,0.9f,1);
        case E_VAMPIRE: return C4(0.8f,0.1f,0.1f,1);
        case E_DRAGON:  return C4(0.9f,0.3f,0.9f,1);
    }
    return C4(0.5f,0.5f,0.5f,1);
}
static const char *EName(EnemyType t){
    switch(t){
        case E_GOBLIN:  return "Goblin";
        case E_ORC:     return "Orc";
        case E_TROLL:   return "Troll";
        case E_VAMPIRE: return "Vampiro";
        case E_DRAGON:  return "Dragao";
    }
    return "?";
}
static const char *IName(ItemType t){
    switch(t){
        case I_POTION: return "Pocao de Vida";
        case I_SWORD:  return "Espada Magica";
        case I_SHIELD: return "Escudo de Ferro";
        case I_ARMOR:  return "Armadura";
        case I_FIRE:   return "Pergaminho Fogo";
        case I_ICE:    return "Pergaminho Gelo";
        case I_BOOTS:  return "Botas Magicas";
    }
    return "Item";
}
static int IValue(ItemType t){
    switch(t){
        case I_POTION: return RR(12,22);
        case I_SWORD:  return RR(2,5);
        case I_SHIELD: return RR(2,4);
        case I_ARMOR:  return RR(1,3);
        default:       return 0;
    }
}

void PlaceEnemies(void){
    int cnt=3+plr.floor*2; if(cnt>MAX_ENEMIES) cnt=MAX_ENEMIES;
    for(int i=0;i<MAX_ENEMIES;i++) ens[i].alive=0;
    for(int i=0;i<cnt;i++){
        for(int t=0;t<60;t++){
            int ri=RR(1,roomCnt-1);
            Room r=rooms[ri];
            int ex=r.x+RR(0,r.w-1), ey=r.y+RR(0,r.h-1);
            if(map[ey][ex].type!=TILE_FLOOR) continue;
            if(ex==plr.x&&ey==plr.y) continue;

            /* Distribuição por andar: cada andar desloca o peso para
               inimigos mais fortes, mas sem permitir Dragão/Vampiro
               cedo demais nem garantir Dragão constante no andar 5. */
            int roll=RR(1,100);
            EnemyType et;
            switch(plr.floor){
                case 1:
                    if(roll<=70)      et=E_GOBLIN;
                    else if(roll<=95) et=E_ORC;
                    else              et=E_TROLL;
                    break;
                case 2:
                    if(roll<=45)      et=E_GOBLIN;
                    else if(roll<=80) et=E_ORC;
                    else if(roll<=95) et=E_TROLL;
                    else              et=E_VAMPIRE;
                    break;
                case 3:
                    if(roll<=25)      et=E_GOBLIN;
                    else if(roll<=55) et=E_ORC;
                    else if(roll<=80) et=E_TROLL;
                    else if(roll<=95) et=E_VAMPIRE;
                    else              et=E_DRAGON;
                    break;
                case 4:
                    if(roll<=15)      et=E_GOBLIN;
                    else if(roll<=40) et=E_ORC;
                    else if(roll<=65) et=E_TROLL;
                    else if(roll<=88) et=E_VAMPIRE;
                    else              et=E_DRAGON;
                    break;
                default: /* andar 5+ */
                    if(roll<=10)      et=E_GOBLIN;
                    else if(roll<=30) et=E_ORC;
                    else if(roll<=55) et=E_TROLL;
                    else if(roll<=80) et=E_VAMPIRE;
                    else              et=E_DRAGON;
                    break;
            }

            Enemy *e=&ens[i];
            e->type=et; e->x=ex; e->y=ey; e->alive=1;
            e->alerted=0; e->moveTimer=0; e->frozenT=0;
            e->col=ECol(et); e->name=EName(et);
            int b=(int)et;
            /* Crescimento por andar suavizado: metade do que era antes,
               evita inimigos absurdamente fortes no andar 5. */
            e->maxHp=10+b*7+plr.floor*2; e->hp=e->maxHp;
            e->atk=3+b*2+plr.floor/2; e->def=b+plr.floor/3;
            e->exp=5+b*8+plr.floor*2;
            e->moveDelay=26-b*2; if(e->moveDelay<10) e->moveDelay=10;
            /* ritmo de ataque corpo-a-corpo: bem mais rapido que o
               deslocamento. Goblin ataca quase todo turno; Dragao é
               mais lento para golpear mas ainda assim ameaçador. */
            e->atkDelay=1+b; if(e->atkDelay>4) e->atkDelay=4;
            break;
        }
    }
}

void PlaceItems(void){
    int cnt=4+RR(0,2)+plr.floor/2; if(cnt>MAX_ITEMS) cnt=MAX_ITEMS;
    for(int i=0;i<MAX_ITEMS;i++) its[i].active=0;
    for(int i=0;i<cnt;i++){
        for(int t=0;t<60;t++){
            int ri=RR(0,roomCnt-1);
            Room r=rooms[ri];
            int ix=r.x+RR(0,r.w-1), iy=r.y+RR(0,r.h-1);
            if(map[iy][ix].type!=TILE_FLOOR) continue;
            ItemType it=(ItemType)RR(0,6);
            its[i].type=it; its[i].mx=ix; its[i].my=iy;
            its[i].active=1; its[i].name=IName(it); its[i].value=IValue(it);
            break;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════
   FOV
═══════════════════════════════════════════════════════════════ */
int HasLOS(int x0,int y0,int x1,int y1){
    int dx=abs(x1-x0),dy=abs(y1-y0);
    int sx=x0<x1?1:-1, sy=y0<y1?1:-1, err=dx-dy;
    while(x0!=x1||y0!=y1){
        if(x0<0||x0>=MAP_W||y0<0||y0>=MAP_H) return 0;
        if(map[y0][x0].type==TILE_WALL) return 0;
        int e2=2*err;
        if(e2>-dy){err-=dy;x0+=sx;}
        if(e2< dx){err+=dx;y0+=sy;}
    }
    return 1;
}

void UpdateFOV(void){
    for(int y=0;y<MAP_H;y++)
        for(int x=0;x<MAP_W;x++)
            map[y][x].visible=0;
    for(int y=plr.y-FOV_R;y<=plr.y+FOV_R;y++)
        for(int x=plr.x-FOV_R;x<=plr.x+FOV_R;x++){
            if(x<0||x>=MAP_W||y<0||y>=MAP_H) continue;
            if(abs(x-plr.x)+abs(y-plr.y)>FOV_R) continue;
            if(HasLOS(plr.x,plr.y,x,y)){
                map[y][x].visible=1;
                map[y][x].explored=1;
            }
        }
}

/* ═══════════════════════════════════════════════════════════════
   LÓGICA
═══════════════════════════════════════════════════════════════ */
int IsWalkable(int x,int y){
    if(x<0||x>=MAP_W||y<0||y>=MAP_H) return 0;
    return map[y][x].type!=TILE_WALL;
}

int EnemyAt(int x,int y){
    for(int i=0;i<MAX_ENEMIES;i++)
        if(ens[i].alive&&ens[i].x==x&&ens[i].y==y) return i;
    return -1;
}

void AddLog(const char *m){
    if(logCnt<MAX_LOG){ strncpy(logMsg[logCnt++],m,159); }
    else{
        for(int i=0;i<MAX_LOG-1;i++) strncpy(logMsg[i],logMsg[i+1],159);
        strncpy(logMsg[MAX_LOG-1],m,159);
    }
}

void SpawnParts(int tx,int ty,Col4 c,int n){
    float ox=10+(float)tx*TILE+TILE/2.0f;
    float oy=10+(float)ty*TILE+TILE/2.0f;
    for(int i=0;i<MAX_PART;i++){
        if(pts[i].life>0) continue;
        if(n--<=0) break;
        pts[i].x=ox; pts[i].y=oy;
        pts[i].vx=(float)RR(-50,50)/10.0f;
        pts[i].vy=(float)RR(-60,20)/10.0f;
        pts[i].col=c;
        pts[i].maxLife=0.3f+(float)RR(0,40)/100.0f;
        pts[i].life=pts[i].maxLife;
        pts[i].sz=RR(2,5);
    }
}

void UpdateParts(void){
    for(int i=0;i<MAX_PART;i++){
        if(pts[i].life<=0) continue;
        pts[i].x+=pts[i].vx;
        pts[i].y+=pts[i].vy;
        pts[i].vy+=0.3f;
        pts[i].life-=(float)dt;
    }
}

void ScreenShake(float mag,float dur){ shakeMag=mag; shakeT=dur; }

void LevelUp(void){
    plr.level++;
    plr.exp-=plr.expNext;
    plr.expNext=plr.level*25;
    plr.maxHp+=8; plr.hp+=8;
    plr.atk+=2; plr.def+=1;
    char buf[80];
    snprintf(buf,sizeof(buf),"LEVEL UP! Nivel %d!",plr.level);
    AddLog(buf);
    PlaySfx(SFX_LEVELUP);
    SpawnParts(plr.x,plr.y,C4(1,1,0,1),20);
    score+=plr.level*100;
}

void TryPickup(void){
    for(int i=0;i<MAX_ITEMS;i++){
        if(!its[i].active) continue;
        if(its[i].mx!=plr.x||its[i].my!=plr.y) continue;
        if(plr.invCount>=MAX_INV){ AddLog("Inventario cheio!"); return; }
        plr.inv[plr.invCount++]=its[i];
        its[i].active=0;
        char buf[80]; snprintf(buf,sizeof(buf),"Pegou: %s",its[i].name);
        AddLog(buf);
        PlaySfx(SFX_PICKUP);
        SpawnParts(plr.x,plr.y,C4(0.3f,0.8f,1,1),8);
        return;
    }
}

void UseItem(int slot){
    if(slot<0||slot>=plr.invCount) return;
    Item it=plr.inv[slot];
    char buf[120];

    switch(it.type){
        case I_POTION:
            plr.hp+=it.value; if(plr.hp>plr.maxHp) plr.hp=plr.maxHp;
            snprintf(buf,sizeof(buf),"Usou Pocao! +%d HP",it.value);
            PlaySfx(SFX_POTION);
            SpawnParts(plr.x,plr.y,C4(0.2f,1,0.2f,1),12);
            break;
        case I_SWORD:
            plr.atkBonus+=it.value;
            snprintf(buf,sizeof(buf),"Espada! +%d ATK",it.value);
            PlaySfx(SFX_PICKUP);
            SpawnParts(plr.x,plr.y,C4(1,0.6f,0,1),10);
            break;
        case I_SHIELD:
            if(!plr.hasShield){
                plr.defBonus+=it.value; plr.hasShield=1;
                snprintf(buf,sizeof(buf),"Escudo! +%d DEF",it.value);
                PlaySfx(SFX_PICKUP);
                SpawnParts(plr.x,plr.y,C4(0.2f,0.4f,1,1),10);
            } else snprintf(buf,sizeof(buf),"Ja tem escudo!");
            break;
        case I_ARMOR:
            plr.defBonus+=it.value;
            snprintf(buf,sizeof(buf),"Armadura! +%d DEF",it.value);
            PlaySfx(SFX_PICKUP);
            SpawnParts(plr.x,plr.y,C4(0.6f,0.6f,0.6f,1),8);
            break;
        case I_FIRE:{
            int killed=0;
            for(int i=0;i<MAX_ENEMIES;i++){
                if(!ens[i].alive) continue;
                int d=abs(ens[i].x-plr.x)+abs(ens[i].y-plr.y);
                if(d<=3){
                    int dmg=RR(15,30); ens[i].hp-=dmg;
                    SpawnParts(ens[i].x,ens[i].y,C4(1,0.3f,0,1),14);
                    if(ens[i].hp<=0){ ens[i].alive=0; killed++; PlaySfx(SFX_KILL); }
                }
            }
            PlaySfx(SFX_HIT);
            snprintf(buf,sizeof(buf),"FOGO! %d inimigos atingidos!",killed);
            ScreenShake(5,0.4f);
            break;
        }
        case I_ICE:
            for(int i=0;i<MAX_ENEMIES;i++){
                if(!ens[i].alive) continue;
                if(map[ens[i].y][ens[i].x].visible)
                    ens[i].frozenT=8; /* 8 turnos parado */
            }
            PlaySfx(SFX_PICKUP);
            snprintf(buf,sizeof(buf),"GELO! Inimigos congelados!");
            SpawnParts(plr.x,plr.y,C4(0.5f,0.9f,1,1),20);
            break;
        case I_BOOTS:
            plr.hasBoots=1;
            snprintf(buf,sizeof(buf),"Botas! Armadilhas reveladas!");
            SpawnParts(plr.x,plr.y,C4(0.8f,0.5f,0.2f,1),8);
            break;
        default: snprintf(buf,sizeof(buf),"Item desconhecido."); break;
    }
    AddLog(buf);
    for(int i=slot;i<plr.invCount-1;i++) plr.inv[i]=plr.inv[i+1];
    plr.invCount--;
}

void PlrAttack(int ex,int ey){
    int ei=EnemyAt(ex,ey); if(ei<0) return;
    Enemy *e=&ens[ei];
    int dmg=plr.atk+plr.atkBonus+RR(0,3);
    int actual=dmg-e->def; if(actual<1) actual=1;
    e->hp-=actual; e->alerted=1;
    char buf[80]; snprintf(buf,sizeof(buf),"Voce ataca %s por %d!",e->name,actual);
    AddLog(buf);
    PlaySfx(SFX_SWING); PlaySfx(SFX_HIT);
    SpawnParts(ex,ey,C4(1,0.6f,0,1),8);
    ScreenShake(2,0.15f); score+=actual;
    if(e->hp<=0){
        e->alive=0; plr.exp+=e->exp;
        snprintf(buf,sizeof(buf),"%s morto! +%d EXP",e->name,e->exp);
        AddLog(buf);
        PlaySfx(SFX_KILL);
        SpawnParts(ex,ey,C4(1,0.85f,0,1),18); score+=e->exp*2;
        if(plr.exp>=plr.expNext) LevelUp();
    }
}

void EnemyAttack(Enemy *e){
    int dmg=e->atk+RR(0,3);
    int def=plr.def+plr.defBonus;
    int actual=dmg-def; if(actual<1) actual=1;
    if(e->type==E_VAMPIRE && e->hp<e->maxHp){
        int heal=actual/2; if(heal<1) heal=1;
        e->hp+=heal; if(e->hp>e->maxHp) e->hp=e->maxHp;
    }
    plr.hp-=actual;
    char buf[80]; snprintf(buf,sizeof(buf),"%s ataca voce por %d!",e->name,actual);
    AddLog(buf);
    PlaySfx(SFX_HURT);
    SpawnParts(plr.x,plr.y,C4(1,0.1f,0.1f,1),10);
    ScreenShake(3,0.2f);
    if(plr.hp<=0){ gs=GS_DEAD; pendingRankInsert=1; AddLog("VOCE MORREU!"); PlaySfx(SFX_DEATH); }
}

void MoveEnemy(Enemy *e){
    if(!e->alive) return;

    if(e->frozenT>0){ e->frozenT--; return; }

    int dist=abs(e->x-plr.x)+abs(e->y-plr.y);

    /* Alerta: visível no FOV OU muito perto (dist<=3) */
    if((map[e->y][e->x].visible&&dist<=8)||dist<=3) e->alerted=1;

    /* Adjacente ao jogador: ataca usando um ritmo de combate proprio
       (atkDelay), bem mais rapido que o deslocamento (moveDelay).
       Isso garante combate desafiador mesmo para inimigos lentos,
       enquanto preserva a chance de fuga ao se afastar de um Troll/Dragao. */
    if(dist==1){
        e->moveTimer++;
        if(e->moveTimer<e->atkDelay) return;
        e->moveTimer=0;
        EnemyAttack(e);
        return;
    }

    /* Para movimento, respeita o delay */
    e->moveTimer++;
    if(e->moveTimer<e->moveDelay) return;
    e->moveTimer=0;

    if(!e->alerted){
        if(RR(0,3)==0){
            int D[4][2]={{0,-1},{0,1},{-1,0},{1,0}};
            int d=RR(0,3);
            int nx=e->x+D[d][0], ny=e->y+D[d][1];
            if(IsWalkable(nx,ny)&&EnemyAt(nx,ny)<0&&!(nx==plr.x&&ny==plr.y))
                { e->x=nx; e->y=ny; }
        }
        return;
    }
    int best=9999,bx=e->x,by=e->y;
    int D[4][2]={{0,-1},{0,1},{-1,0},{1,0}};
    for(int d=0;d<4;d++){
        int nx=e->x+D[d][0], ny=e->y+D[d][1];
        if(!IsWalkable(nx,ny)) continue;
        if(EnemyAt(nx,ny)>=0) continue;
        if(nx==plr.x&&ny==plr.y){ EnemyAttack(e); return; }
        int nd=abs(nx-plr.x)+abs(ny-plr.y);
        if(nd<best){ best=nd; bx=nx; by=ny; }
    }
    e->x=bx; e->y=by;
}

void EnemyTurn(void){
    for(int i=0;i<MAX_ENEMIES;i++) MoveEnemy(&ens[i]);
}

void MovePlayer(int dx,int dy){
    if(plr.frozen){ plr.frozenT--; if(plr.frozenT<=0) plr.frozen=0;
        AddLog("Voce esta congelado!"); return; }
    int nx=plr.x+dx, ny=plr.y+dy;
    int ei=EnemyAt(nx,ny);
    if(ei>=0){ PlrAttack(nx,ny); EnemyTurn(); turn++; return; }
    if(!IsWalkable(nx,ny)) return;
    plr.x=nx; plr.y=ny;

    /* armadilha */
    if(map[ny][nx].type==TILE_TRAP){
        int dmg=RR(3,8)+plr.floor;
        if(plr.hasBoots) dmg/=2; /* botas absorvem metade do dano */
        int actual=dmg-(plr.def+plr.defBonus); if(actual<1) actual=1;
        plr.hp-=actual;
        char buf[80]; snprintf(buf,sizeof(buf),"ARMADILHA! -%d HP!",actual);
        AddLog(buf);
        PlaySfx(SFX_TRAP);
        ScreenShake(4,0.3f);
        SpawnParts(plr.x,plr.y,C4(1,0,0,1),12);
        map[ny][nx].type=TILE_FLOOR;
        if(plr.hp<=0){ gs=GS_DEAD; pendingRankInsert=1; PlaySfx(SFX_DEATH); return; }
    }

    /* escada */
    if(map[ny][nx].type==TILE_STAIRS){
        plr.floor++;
        if(plr.floor>5){ gs=GS_WIN; score+=plr.level*500; pendingRankInsert=1; PlaySfx(SFX_WIN); return; }
        char buf[60]; snprintf(buf,sizeof(buf),"Andar %d! Descendo...",plr.floor);
        AddLog(buf);
        PlaySfx(SFX_STAIRS);
        int heal=(int)((float)plr.hp*0.1f);
        plr.hp+=heal; if(plr.hp>plr.maxHp) plr.hp=plr.maxHp;
        GenDungeon(); return;
    }

    TryPickup();
    UpdateFOV();
    EnemyTurn();
    turn++;
}

/* ═══════════════════════════════════════════════════════════════
   RANKING  —  persistido em arquivo texto (ranking.txt)
   Formato por linha: nome;score;level;floor;turns;won
═══════════════════════════════════════════════════════════════ */
void RankLoad(void){
    rankCnt=0;
    FILE *f=fopen(RANK_FILE,"r");
    if(!f) return;
    char line[256];
    while(rankCnt<MAX_RANK && fgets(line,sizeof(line),f)){
        RankEntry *r=&rankings[rankCnt];
        char *tok=strtok(line,";");
        if(!tok) continue;
        strncpy(r->name,tok,NAME_MAXLEN); r->name[NAME_MAXLEN]='\0';
        tok=strtok(NULL,";"); if(!tok) continue; r->score=atoi(tok);
        tok=strtok(NULL,";"); if(!tok) continue; r->level=atoi(tok);
        tok=strtok(NULL,";"); if(!tok) continue; r->floor=atoi(tok);
        tok=strtok(NULL,";"); if(!tok) continue; r->turns=atoi(tok);
        tok=strtok(NULL,";\n"); if(!tok) continue; r->won=atoi(tok);
        rankCnt++;
    }
    fclose(f);
}

void RankSave(void){
    FILE *f=fopen(RANK_FILE,"w");
    if(!f) return;
    for(int i=0;i<rankCnt;i++){
        RankEntry *r=&rankings[i];
        fprintf(f,"%s;%d;%d;%d;%d;%d\n",
                r->name,r->score,r->level,r->floor,r->turns,r->won);
    }
    fclose(f);
}

void RankInsert(const char *name,int sc,int level,int floor,int turns,int won){
    RankEntry ne;
    strncpy(ne.name,name,NAME_MAXLEN); ne.name[NAME_MAXLEN]='\0';
    if(ne.name[0]=='\0') strcpy(ne.name,"ANONIMO");
    ne.score=sc; ne.level=level; ne.floor=floor; ne.turns=turns; ne.won=won;

    /* acha posição ordenada por score desc */
    int pos=rankCnt;
    for(int i=0;i<rankCnt;i++){
        if(ne.score>rankings[i].score){ pos=i; break; }
    }
    if(pos>=MAX_RANK) return; /* não entra no top */

    int last=rankCnt<MAX_RANK?rankCnt:MAX_RANK-1;
    for(int i=last;i>pos;i--) rankings[i]=rankings[i-1];
    rankings[pos]=ne;
    if(rankCnt<MAX_RANK) rankCnt++;

    RankSave();
}

/* ═══════════════════════════════════════════════════════════════
   RESET
═══════════════════════════════════════════════════════════════ */
void ResetGame(void){
    memset(pts,0,sizeof(pts));
    logCnt=0; score=0; turn=0; pendingRankInsert=0;
    plr.hp=plr.maxHp=30; plr.atk=5; plr.def=2;
    plr.level=1; plr.exp=0; plr.expNext=20; plr.floor=1;
    plr.invCount=0; plr.atkBonus=0; plr.defBonus=0;
    plr.hasShield=0; plr.hasBoots=0; plr.frozen=0; plr.frozenT=0;
    GenDungeon();
    AddLog("Bem-vindo ao Dungeon of Doom!");
    AddLog("WASD=mover | 1-5=item | ESC=sair");
    gs=GS_PLAY;
}

/* ═══════════════════════════════════════════════════════════════
   DESENHO DO JOGO
═══════════════════════════════════════════════════════════════ */

/* offset de câmera por shake */
static float shOX=0,shOY=0;

void DrawMap(void){
    float ox=10+shOX, oy=10+shOY;
    float T=(float)TILE;

    for(int y=0;y<MAP_H;y++)
    for(int x=0;x<MAP_W;x++){
        Tile *t=&map[y][x];
        if(!t->explored) continue;
        float px=ox+x*T, py=oy+y*T;
        float dim=t->visible?1.0f:0.28f;

        switch(t->type){
            case TILE_WALL:
                DrawRect(px,py,T,T,C4(0.25f*dim,0.25f*dim,0.35f*dim,1));
                if(t->visible){
                    /* borda decorativa */
                    glColor4f(0.35f,0.35f,0.5f,1);
                    glLineWidth(1);
                    glBegin(GL_LINE_LOOP);
                      glVertex2f(px,py); glVertex2f(px+T,py);
                      glVertex2f(px+T,py+T); glVertex2f(px,py+T);
                    glEnd();
                }
                break;
            case TILE_FLOOR:
                DrawRect(px,py,T,T,C4(0.12f*dim,0.12f*dim,0.18f*dim,1));
                /* pontilhado de piso */
                if(t->visible)
                    DrawCircleFill(px+T/2,py+T/2,1.2f,C4(0.2f,0.2f,0.3f,1),6);
                break;
            case TILE_STAIRS:
                DrawRect(px,py,T,T,C4(0.12f*dim,0.12f*dim,0.18f*dim,1));
                if(t->visible){
                    /* escada dourada */
                    Col4 gc=C4(1,0.8f,0,dim);
                    float margin=4;
                    for(int step=0;step<4;step++){
                        float sy2=py+margin+step*(T-2*margin)/4;
                        DrawRect(px+margin,sy2,T-2*margin,3,gc);
                    }
                    DrawDiamond(px+T/2-4,py+T/2-4,8,C4(1,1,0.5f,1));
                }
                break;
            case TILE_TRAP:
                DrawRect(px,py,T,T,C4(0.12f*dim,0.12f*dim,0.18f*dim,1));
                if(t->visible&&plr.hasBoots){
                    DrawTriangle(px+4,py+4,T-8,C4(1,0.1f,0.1f,dim));
                }
                break;
        }
    }

    /* itens */
    for(int i=0;i<MAX_ITEMS;i++){
        if(!its[i].active) continue;
        if(!map[its[i].my][its[i].mx].visible) continue;
        float px=ox+its[i].mx*T+T/2.0f;
        float py=oy+its[i].my*T+T/2.0f;
        DrawDiamond(px-5,py-5,10,C4(0.3f,0.8f,1,1));
        DrawCircleFill(px,py,3,C4(0.6f,0.95f,1,1),8);
    }

    /* inimigos */
    for(int i=0;i<MAX_ENEMIES;i++){
        if(!ens[i].alive) continue;
        if(!map[ens[i].y][ens[i].x].visible) continue;
        float px=ox+ens[i].x*T, py=oy+ens[i].y*T;
        float m=3;
        /* corpo */
        DrawRect(px+m,py+m,T-m*2,T-m*2,ens[i].col);
        /* olhos */
        DrawCircleFill(px+T*0.35f,py+T*0.38f,2,C4(0,0,0,1),6);
        DrawCircleFill(px+T*0.65f,py+T*0.38f,2,C4(0,0,0,1),6);
        /* barra HP */
        float hpr=(float)ens[i].hp/(float)ens[i].maxHp;
        DrawBar(px,py-5,T,3,1,C4(0.15f,0.15f,0.15f,1),C4(0,0,0,1));
        DrawBar(px,py-5,T,3,hpr,C4(0.15f,0.15f,0.15f,1),
                hpr>0.5f?C4(0.1f,0.9f,0.1f,1):C4(0.9f,0.1f,0.1f,1));
    }

    /* jogador */
    {
        /* piscar quando HP baixo */
        int blink=(plr.hp<plr.maxHp/4)&&((turn/4)%2==0);
        if(!blink){
            float px=ox+plr.x*T, py=oy+plr.y*T;
            Col4 body=plr.frozen?C4(0.5f,0.8f,1,1):C4(0.2f,0.8f,0.4f,1);
            float m=3;
            DrawRect(px+m,py+m,T-m*2,T-m*2,body);
            /* "face" do jogador */
            DrawCircleFill(px+T*0.35f,py+T*0.38f,2.2f,C4(0,0,0,1),6);
            DrawCircleFill(px+T*0.65f,py+T*0.38f,2.2f,C4(0,0,0,1),6);
            /* sorriso */
            glColor4f(0,0,0,1); glLineWidth(1.5f);
            glBegin(GL_LINE_STRIP);
              for(int s=0;s<=8;s++){
                  float a=(float)s/8.0f*3.14159f;
                  glVertex2f(px+T*0.3f+cosf(a)*T*0.2f, py+T*0.65f-sinf(a)*T*0.15f);
              }
            glEnd();
        }
    }
}

/* ─── HUD (painel direito) ────────────────────────────────── */
void DrawHUD(void){
    float hx=(float)(MAP_W*TILE)+20;
    float hy=10;
    float hw=(float)(SW)-hx-8;
    float lh=18;

    /* fundo */
    DrawRectBorder(hx-4,hy,hw+8,SH-20,C4(0.07f,0.07f,0.13f,1),
                   1.5f,C4(0.3f,0.3f,0.5f,1));

    float tx=hx+4;
    Col4 gold=C4(1,0.85f,0,1);
    Col4 white=C4(0.9f,0.9f,0.9f,1);
    Col4 gray=C4(0.5f,0.5f,0.6f,1);
    Col4 red=C4(1,0.2f,0.2f,1);
    Col4 blue=C4(0.3f,0.6f,1,1);
    Col4 cyan=C4(0.3f,0.9f,1,1);

    DrawString(tx,hy+6, 8,"DUNGEON OF DOOM",gold);
    hy+=24;

    char buf[80];
    snprintf(buf,sizeof(buf),"Andar: %d/5   Turno: %d",plr.floor,turn);
    DrawString(tx,hy,6,buf,gray); hy+=lh;
    snprintf(buf,sizeof(buf),"Score: %d",score);
    DrawString(tx,hy,6,buf,gold); hy+=lh+4;

    /* separador */
    DrawRect(tx,hy,hw-4,1,C4(0.3f,0.3f,0.5f,1)); hy+=6;

    DrawString(tx,hy,7,"JOGADOR",white); hy+=lh;

    snprintf(buf,sizeof(buf),"HP %d/%d",plr.hp,plr.maxHp);
    DrawString(tx,hy,6,buf,white); hy+=13;
    float hpr=(float)plr.hp/(float)plr.maxHp;
    Col4 hpCol=hpr>0.5f?C4(0.1f,0.9f,0.1f,1):(hpr>0.25f?C4(1,0.8f,0,1):red);
    DrawBar(tx,hy,hw-8,9,hpr,C4(0.15f,0.15f,0.15f,1),hpCol); hy+=14;

    float epr=(float)plr.exp/(float)plr.expNext;
    snprintf(buf,sizeof(buf),"Nivel %d  EXP %d/%d",plr.level,plr.exp,plr.expNext);
    DrawString(tx,hy,6,buf,white); hy+=13;
    DrawBar(tx,hy,hw-8,7,epr,C4(0.15f,0.15f,0.15f,1),C4(0.6f,0.2f,1,1)); hy+=13;

    snprintf(buf,sizeof(buf),"ATK %d+%d   DEF %d+%d",
             plr.atk,plr.atkBonus,plr.def,plr.defBonus);
    DrawString(tx,hy,6,buf,gray); hy+=lh;

    if(plr.frozen){
        snprintf(buf,sizeof(buf),"[CONGELADO %d]",plr.frozenT);
        DrawString(tx,hy,6,buf,cyan); hy+=lh;
    }

    hy+=4; DrawRect(tx,hy,hw-4,1,C4(0.3f,0.3f,0.5f,1)); hy+=6;
    DrawString(tx,hy,7,"INVENTARIO",white); hy+=lh;
    DrawString(tx,hy,5,"Use teclas 1-5",gray); hy+=13;

    for(int i=0;i<plr.invCount;i++){
        snprintf(buf,sizeof(buf),"[%d] %s",i+1,plr.inv[i].name);
        DrawString(tx,hy,6,buf,cyan); hy+=14;
    }
    if(plr.invCount==0){ DrawString(tx,hy,6,"(vazio)",gray); hy+=14; }

    hy+=4; DrawRect(tx,hy,hw-4,1,C4(0.3f,0.3f,0.5f,1)); hy+=6;
    DrawString(tx,hy,7,"CONTROLES",white); hy+=lh;
    const char *ctrls[]={"WASD - Mover/Atacar","ESPACO - Atacar adj.",
                         "1-5 - Usar item","ESC - Sair"};
    for(int i=0;i<4;i++){ DrawString(tx,hy,5,ctrls[i],gray); hy+=13; }

    /* legenda dos inimigos */
    hy+=4; DrawRect(tx,hy,hw-4,1,C4(0.3f,0.3f,0.5f,1)); hy+=6;
    DrawString(tx,hy,6,"INIMIGOS VIVOS",white); hy+=lh;
    for(int i=0;i<MAX_ENEMIES;i++){
        if(!ens[i].alive) continue;
        DrawCircleFill(tx+6,hy+6,5,ens[i].col,8);
        snprintf(buf,sizeof(buf),"  %s %d/%d",ens[i].name,ens[i].hp,ens[i].maxHp);
        DrawString(tx+2,hy,5,buf,gray);
        hy+=12; if(hy>SH-60) break;
    }
}

/* ─── Painel de Log ───────────────────────────────────────── */
void DrawLogPanel(void){
    float lx=10, ly=(float)(MAP_H*TILE)+16;
    float lw=(float)(MAP_W*TILE), lh=(float)(SH)-ly-4;
    DrawRectBorder(lx,ly,lw,lh,C4(0.05f,0.05f,0.1f,1),
                   1,C4(0.3f,0.3f,0.5f,1));

    for(int i=0;i<logCnt;i++){
        float age=(float)(logCnt-1-i)/(float)MAX_LOG;
        float al=1.0f-age*0.6f;
        Col4 c=C4(0.8f,0.8f,0.8f,al);
        if(strstr(logMsg[i],"MORREU")||strstr(logMsg[i],"morto"))
            c=C4(1,0.3f,0.3f,al);
        else if(strstr(logMsg[i],"LEVEL")||strstr(logMsg[i],"Pegou"))
            c=C4(1,0.9f,0.2f,al);
        else if(strstr(logMsg[i],"ataca voce"))
            c=C4(1,0.5f,0.2f,al);
        DrawString(lx+6, ly+6+(float)i*15, 6, logMsg[i], c);
    }
}

/* ─── Partículas ─────────────────────────────────────────── */
void DrawParticles(void){
    for(int i=0;i<MAX_PART;i++){
        if(pts[i].life<=0) continue;
        float al=pts[i].life/pts[i].maxLife;
        Col4 c=C4fade(pts[i].col,al);
        DrawCircleFill(pts[i].x+shOX,pts[i].y+shOY,
                       (float)pts[i].sz*al,c,8);
    }
}

/* ─── Menu Principal ─────────────────────────────────────── */
void DrawMenu(void){
    menuTick++;
    float W=(float)SW, H=(float)SH;

    /* fundo animado */
    DrawRect(0,0,W,H,C4(0.04f,0.04f,0.08f,1));
    for(int y=0;y<SH;y+=28)
    for(int x=0;x<SW;x+=28){
        float t=sinf((float)(menuTick+x+y)*0.025f)*0.5f+0.5f;
        DrawCircleFill((float)x,(float)y,1.5f,C4(0,0,t*0.3f,1),5);
    }

    /* título com sombra */
    float tx=(float)SW/2.0f-180;
    DrawString(tx+3,143,20,"DUNGEON OF DOOM",C4(0.4f,0,0,1));
    DrawString(tx,140,20,"DUNGEON OF DOOM",C4(1,0.2f,0.2f,1));

    DrawString((float)SW/2-130,195,8,"",
               C4(0.6f,0.6f,0.8f,1));

    DrawRect(tx-10,230,380,1,C4(0.4f,0.1f,0.1f,1));

    /* instruções */
    const char *how[]={
        "Como jogar:",
        "WASD / Setas  -  Mover e atacar inimigos adjacentes",
        "ESPACO        -  Atacar inimigo mais proximo",
        "1 a 5         -  Usar item do inventario",
        "ESC           -  Sair do jogo",
    };
    for(int i=0;i<5;i++)
        DrawString(tx-10,245+(float)i*18,7,how[i],
                   i==0?C4(1,0.85f,0,1):C4(0.7f,0.7f,0.8f,1));

    /* inimigos */
    DrawString(tx-10,345,7,"Inimigos:",C4(1,0.85f,0,1));
    EnemyType types[]={E_GOBLIN,E_ORC,E_TROLL,E_VAMPIRE,E_DRAGON};
    const char *tnames[]={"Goblin","Orc","Troll","Vampiro","Dragao"};
    for(int i=0;i<5;i++){
        DrawCircleFill(tx+i*72+4,366,7,ECol(types[i]),10);
        DrawString(tx+i*72-8,376,5,tnames[i],C4(0.7f,0.7f,0.8f,1));
    }

    DrawString(tx-10,410,6,"Chegue ao andar 5 e suba as escadas para vencer!",
               C4(0.9f,0.9f,0.3f,1));
    DrawString(tx-10,430,6,"Os inimigos ficam mais fortes a cada andar.",
               C4(0.7f,0.4f,0.4f,1));

    /* blink ENTER */
    if((menuTick/30)%2==0)
        DrawString((float)SW/2-160,500,10,"[ ENTER ] INICIAR JOGO",
                   C4(0.3f,1,0.4f,1));
    DrawString((float)SW/2-90,530,7,"[ T ] Ver Ranking",C4(0.6f,0.8f,1,1));

    DrawString(10,(float)SH-16,5,"v2.1  Dungeon of Doom  OpenGL+GLFW",
               C4(0.3f,0.3f,0.4f,1));
}

/* ─── Tela de digitação de nome ──────────────────────────────── */
void DrawNameInput(void){
    DrawRect(0,0,(float)SW,(float)SH,C4(0.04f,0.04f,0.08f,1));
    float cx=(float)SW/2;
    DrawString(cx-170,180,12,"QUAL O SEU NOME, AVENTUREIRO?",C4(1,0.85f,0,1));

    /* caixa de texto */
    float bw=300,bh=40,bx=cx-bw/2,by=260;
    DrawRectBorder(bx,by,bw,bh,C4(0.1f,0.1f,0.18f,1),2,C4(0.5f,0.5f,0.8f,1));
    DrawString(bx+10,by+12,8,playerName,C4(1,1,1,1));

    /* cursor piscante */
    if((menuTick/15)%2==0){
        DrawRect(bx+10+(float)nameLen*8.8f,by+12,3,18,C4(1,1,1,0.8f));
    }

    DrawString(cx-190,330,6,"Digite seu nome (max 15 letras) e aperte ENTER",
               C4(0.7f,0.7f,0.8f,1));
    DrawString(cx-150,355,6,"BACKSPACE apaga   ESC volta ao menu",
               C4(0.6f,0.6f,0.7f,1));
    menuTick++;
}

/* ─── Tela de Ranking ─────────────────────────────────────────── */
void DrawRanking(void){
    DrawRect(0,0,(float)SW,(float)SH,C4(0.03f,0.03f,0.07f,1));
    float cx=(float)SW/2;
    DrawString(cx-130,50,14,"RANKING - TOP 10",C4(1,0.85f,0,1));

    float tx=cx-340, ty=120;
    DrawString(tx,ty,6,"#",C4(0.6f,0.6f,0.8f,1));
    DrawString(tx+40,ty,6,"NOME",C4(0.6f,0.6f,0.8f,1));
    DrawString(tx+260,ty,6,"SCORE",C4(0.6f,0.6f,0.8f,1));
    DrawString(tx+380,ty,6,"NIVEL",C4(0.6f,0.6f,0.8f,1));
    DrawString(tx+480,ty,6,"ANDAR",C4(0.6f,0.6f,0.8f,1));
    DrawString(tx+590,ty,6,"RESULTADO",C4(0.6f,0.6f,0.8f,1));
    ty+=10;
    DrawRect(tx,ty,680,1,C4(0.3f,0.3f,0.5f,1));
    ty+=14;

    if(rankCnt==0){
        DrawString(cx-160,ty+20,7,"Nenhum resultado registrado ainda.",
                   C4(0.6f,0.6f,0.7f,1));
    }
    for(int i=0;i<rankCnt;i++){
        RankEntry *r=&rankings[i];
        char buf[16];
        Col4 c=(i==0)?C4(1,0.85f,0,1):(i<3?C4(0.8f,0.85f,1,1):C4(0.8f,0.8f,0.8f,1));
        snprintf(buf,sizeof(buf),"%d",i+1);
        DrawString(tx,ty,6,buf,c);
        DrawString(tx+40,ty,6,r->name,c);
        snprintf(buf,sizeof(buf),"%d",r->score);
        DrawString(tx+260,ty,6,buf,c);
        snprintf(buf,sizeof(buf),"%d",r->level);
        DrawString(tx+380,ty,6,buf,c);
        snprintf(buf,sizeof(buf),"%d/5",r->floor);
        DrawString(tx+480,ty,6,buf,c);
        DrawString(tx+590,ty,6,r->won?"VITORIA":"MORTO",
                   r->won?C4(0.3f,1,0.4f,1):C4(1,0.4f,0.4f,1));
        ty+=24;
    }

    DrawString(cx-150,(float)SH-40,7,"[ ESC ] Voltar ao menu",C4(0.8f,0.8f,0.9f,1));
}

/* ─── Tela de Morte ──────────────────────────────────────── */
void DrawDead(void){
    DrawRect(0,0,(float)SW,(float)SH,C4(0.06f,0,0,1));
    float cx=(float)SW/2;
    DrawString(cx-120,180,20,"VOCE MORREU",C4(0.9f,0.1f,0.1f,1));
    char buf[100];
    snprintf(buf,sizeof(buf),"%s  -  Score: %d",playerName,score);
    DrawString(cx-100,260,9,buf,C4(1,0.85f,0,1));
    snprintf(buf,sizeof(buf),"Nivel: %d   Andar: %d   Turno: %d",plr.level,plr.floor,turn);
    DrawString(cx-160,305,7,buf,C4(0.7f,0.7f,0.7f,1));
    DrawString(cx-150,345,6,"Resultado salvo no ranking!",C4(0.4f,0.9f,1,1));
    DrawString(cx-220,400,8,"[ R ] Tentar novamente   [ T ] Ranking   [ ESC ] Sair",
               C4(0.9f,0.9f,0.9f,1));
}

/* ─── Tela de Vitória ────────────────────────────────────── */
void DrawWin(void){
    DrawRect(0,0,(float)SW,(float)SH,C4(0,0.06f,0,1));
    float cx=(float)SW/2;
    DrawString(cx-120,180,20,"VOCE VENCEU!",C4(0.9f,0.9f,0.1f,1));
    DrawString(cx-140,250,8,"O Dungeon foi conquistado!",C4(0.5f,1,0.5f,1));
    char buf[100];
    snprintf(buf,sizeof(buf),"%s  -  Score Final: %d",playerName,score);
    DrawString(cx-110,300,9,buf,C4(1,1,1,1));
    snprintf(buf,sizeof(buf),"Nivel: %d   Turnos: %d",plr.level,turn);
    DrawString(cx-100,345,7,buf,C4(0.7f,0.7f,0.7f,1));
    DrawString(cx-150,375,6,"Resultado salvo no ranking!",C4(0.4f,1,0.6f,1));
    DrawString(cx-220,420,8,"[ R ] Jogar novamente   [ T ] Ranking   [ ESC ] Sair",
               C4(0.4f,1,0.5f,1));
}

/* ═══════════════════════════════════════════════════════════════
   FRAME PRINCIPAL
═══════════════════════════════════════════════════════════════ */
void DrawFrame(void){
    glClearColor(0.04f,0.04f,0.08f,1);
    glClear(GL_COLOR_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0,SW,SH,0,-1,1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);

    /* screen shake */
    shOX=0; shOY=0;
    if(shakeT>0){
        shOX=(float)(rand()%((int)shakeMag*2+1))-shakeMag;
        shOY=(float)(rand()%((int)shakeMag*2+1))-shakeMag;
        shakeT-=(float)dt;
        if(shakeT<0) shakeT=0;
    }

    switch(gs){
        case GS_MENU: DrawMenu(); break;
        case GS_NAME: DrawNameInput(); break;
        case GS_PLAY:
            DrawMap();
            DrawHUD();
            DrawLogPanel();
            DrawParticles();
            break;
        case GS_DEAD: DrawDead(); break;
        case GS_WIN:  DrawWin();  break;
        case GS_RANKING: DrawRanking(); break;
    }
}

/* ═══════════════════════════════════════════════════════════════
   INPUT
═══════════════════════════════════════════════════════════════ */
void KeyCB(GLFWwindow *w,int key,int sc,int action,int mods){
    (void)w;(void)sc;(void)mods;
    if(action==GLFW_PRESS){
        if(keyBufN<16) keyBuf[keyBufN++]=key;
    }
}

void CharCB(GLFWwindow *w,unsigned int codepoint){
    (void)w;
    if(gs!=GS_NAME) return;
    if(codepoint<32||codepoint>126) return; /* só ASCII imprimível */
    if(nameLen<NAME_MAXLEN){
        playerName[nameLen++]=(char)codepoint;
        playerName[nameLen]='\0';
    }
}

void ProcessInput(void){
    for(int i=0;i<keyBufN;i++){
        int k=keyBuf[i];
        switch(gs){
            case GS_MENU:
                if(k==GLFW_KEY_ENTER||k==GLFW_KEY_SPACE){
                    nameLen=0; playerName[0]='\0';
                    gs=GS_NAME;
                }
                if(k==GLFW_KEY_T){ RankLoad(); gs=GS_RANKING; }
                if(k==GLFW_KEY_ESCAPE) glfwSetWindowShouldClose(win,1);
                break;
            case GS_NAME:
                if(k==GLFW_KEY_BACKSPACE && nameLen>0){
                    nameLen--; playerName[nameLen]='\0';
                }
                if(k==GLFW_KEY_ENTER){
                    if(nameLen==0){ strcpy(playerName,"ANONIMO"); nameLen=7; }
                    ResetGame();
                }
                if(k==GLFW_KEY_ESCAPE) gs=GS_MENU;
                break;
            case GS_PLAY:
                if(k==GLFW_KEY_ESCAPE) glfwSetWindowShouldClose(win,1);
                if(k==GLFW_KEY_W||k==GLFW_KEY_UP)    MovePlayer(0,-1);
                if(k==GLFW_KEY_S||k==GLFW_KEY_DOWN)  MovePlayer(0, 1);
                if(k==GLFW_KEY_A||k==GLFW_KEY_LEFT)  MovePlayer(-1,0);
                if(k==GLFW_KEY_D||k==GLFW_KEY_RIGHT) MovePlayer( 1,0);
                if(k==GLFW_KEY_SPACE){
                    /* ataca adjacente mais próximo */
                    int D[4][2]={{0,-1},{0,1},{-1,0},{1,0}};
                    for(int d=0;d<4;d++){
                        int ei=EnemyAt(plr.x+D[d][0],plr.y+D[d][1]);
                        if(ei>=0){ PlrAttack(plr.x+D[d][0],plr.y+D[d][1]);
                            EnemyTurn(); turn++; break; }
                    }
                }
                /* itens 1-5 */
                if(k>=GLFW_KEY_1&&k<=GLFW_KEY_5) UseItem(k-GLFW_KEY_1);
                break;
            case GS_DEAD:
                if(k==GLFW_KEY_R) ResetGame();
                if(k==GLFW_KEY_T){ RankLoad(); gs=GS_RANKING; }
                if(k==GLFW_KEY_ESCAPE) glfwSetWindowShouldClose(win,1);
                break;
            case GS_WIN:
                if(k==GLFW_KEY_R){ gs=GS_MENU; }
                if(k==GLFW_KEY_T){ RankLoad(); gs=GS_RANKING; }
                if(k==GLFW_KEY_ESCAPE) glfwSetWindowShouldClose(win,1);
                break;
            case GS_RANKING:
                if(k==GLFW_KEY_ESCAPE) gs=GS_MENU;
                break;
        }
    }
    keyBufN=0;

    /* registra no ranking mesmo sem tecla pressionada nesse frame
       (garante que o resultado seja salvo assim que a tela aparece) */
    if(pendingRankInsert && gs==GS_DEAD){
        RankLoad();
        RankInsert(playerName,score,plr.level,plr.floor,turn,0);
        pendingRankInsert=0;
    }
    if(pendingRankInsert && gs==GS_WIN){
        RankLoad();
        RankInsert(playerName,score,plr.level,5,turn,1);
        pendingRankInsert=0;
    }
}

/* ═══════════════════════════════════════════════════════════════
   MAIN
═══════════════════════════════════════════════════════════════ */
int main(void){
    srand((unsigned int)time(NULL));

    AudioInit();

    if(!glfwInit()){ fprintf(stderr,"GLFW falhou\n"); return 1; }
    glfwWindowHint(GLFW_RESIZABLE,GLFW_FALSE);
    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
const GLFWvidmode* mode = glfwGetVideoMode(monitor);
win=glfwCreateWindow(mode->width,mode->height,"Dungeon of Doom — OpenGL + GLFW",monitor,NULL);
    if(!win){ fprintf(stderr,"Janela falhou\n"); glfwTerminate(); return 1; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);
    glfwSetKeyCallback(win,KeyCB);
    glfwSetCharCallback(win,CharCB);

    RankLoad();
    gs=GS_MENU;
    lastTime=glfwGetTime();

    while(!glfwWindowShouldClose(win)){
        double now=glfwGetTime();
        dt=now-lastTime; lastTime=now;
        if(dt>0.05) dt=0.05;

        glfwPollEvents();
        ProcessInput();
        UpdateParts();

        DrawFrame();
        glfwSwapBuffers(win);
    }

    glfwDestroyWindow(win);
    glfwTerminate();
    AudioShutdown();
    return 0;
}
