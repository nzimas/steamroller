/* Host validation/render harness for the exact firmware extension source.
 * Uses an ordinary byte-array RAM fixture, never physical memory.
 *   --layout              state offsets for the validator (JSON)
 *   <audio.pcm> <first.pcm>
 *       generate one sound of each family (Shift + track button 1..8) and
 *       drain its audition through fm_tick into a simulated DSP FIFO;
 *       audio.pcm: 24-bit little-endian mono, all eight auditions in turn;
 *       first.pcm: the first audition block's 32 words (24-bit big-endian),
 *       compared with the ARM emulation; prints one line per family.
 */
#define FM_HOST 1
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
/* Exercise the firmware memory routines, not Darwin fortify macros. */
#undef memcpy
#undef memset
#undef memcmp
uint8_t fm_host_ram[0x50000];
uint8_t fm_host_itcm[0x4000];
unsigned host_shift, host_view=4;
/* stock storage stubs: callbacks run at once; the written file goes to host_file */
static uint8_t *host_files[128], host_scratch[4096]; static uint32_t host_lens[128]; static unsigned host_reloads, host_auditions, host_deletes;
#include "../firmware/rhythm_fm/extension.c"
static uint32_t dsp_ram[0x40000];
uint32_t dsp_read(uint32_t address) {
    if(address>=0x40000) abort();
    return dsp_ram[address];
}
void dsp_write(uint32_t address,uint32_t word) {
    if(address>=0x40000) abort();
    dsp_ram[address]=word & MASK24;
}
void rhythm_boot_original(void) {}
void rhythm_pad_original(void *c,unsigned p,unsigned v) { (void)c;(void)p;(void)v; }
/* parameter locks run only against the stock sequencer; the host never calls them */
unsigned plock_lanes_original(void *p,unsigned a,unsigned b,unsigned c) { (void)p;(void)a;(void)b;(void)c;abort(); }
unsigned plock_knob_original(void *p,unsigned a,unsigned b,unsigned c) { (void)p;(void)a;(void)b;(void)c;abort(); }
unsigned plock_preview_original(void *p,unsigned a) { (void)p;(void)a;abort(); }
uint8_t *plock_store(void *s,unsigned i) { (void)s;(void)i;abort(); }
unsigned sl_store(void *s,unsigned i) { (void)s;(void)i;abort(); }
struct StoragePathFwd;
unsigned host_storage_delete(const StoragePath *path,storage_cb cb) { unsigned had=host_lens[path->index]!=0; host_lens[path->index]=0; host_deletes++; cb(path,2,had ? 0:5,0,0,0); return 0; }
unsigned host_storage_write(const StoragePath *path,uint32_t off,const void *buf,uint32_t len,storage_cb cb) {
    if(path->index>=128u || off+len>(1u<<20)) abort();
    if(!host_files[path->index]) host_files[path->index]=malloc(1u<<20);
    memcpy(host_files[path->index]+off,buf,len); if(off+len>host_lens[path->index]) host_lens[path->index]=off+len;
    cb(path,1,0,(uint32_t)(uintptr_t)buf,len,0); return 0;
}
unsigned host_storage_close(const StoragePath *path,storage_cb cb) { cb(path,5,0,0,0,0); return 0; }
unsigned host_reads;
unsigned host_storage_read(const StoragePath *path,void *buf,uint32_t len) {
    host_reads++;
    if(path->index>=128u || !host_lens[path->index]) return 5;
    memcpy(buf,host_files[path->index],len<host_lens[path->index] ? len : host_lens[path->index]); return 0;
}
uint8_t *host_scratch_acquire(void) { return host_scratch; }
void host_scratch_release(void) {}
void host_file_changed(const StoragePath *path) { if(path->type!=4) abort(); host_reloads++; }
void host_audition_slot(unsigned slot) { (void)slot; host_auditions++; }
static uint16_t host_frame_buf[42];
uint16_t *host_frame(void *view) { (void)view; return host_frame_buf; }
static void boot(void) {
    memset(dsp_ram,0,sizeof(dsp_ram));
    *HEAP_LIMIT=0x2004f760; *HEAP_CURRENT=0x20007b90;
    fm_boot();
}
/* The DSP side: consume one block if READY (as the consumer does), else miss. */
static int consume(FILE *audio,FILE *first,unsigned *blocks) {
    uint32_t seq=dsp_ram[CTRL+1], slot=dsp_ram[CTRL_READ_SLOT];
    if(dsp_ram[slot]!=1 || dsp_ram[slot+1]!=seq) return 0;
    if((dsp_ram[slot+2]&63u)!=FRAMES) return -1;
    for(unsigned i=0;i<FRAMES;i++) {
        uint32_t v=dsp_ram[slot+3+i];
        if(first && *blocks==0) { unsigned char b[3]={(unsigned char)(v>>16),(unsigned char)(v>>8),(unsigned char)v}; fwrite(b,1,3,first); }
        unsigned char b[3]={(unsigned char)v,(unsigned char)(v>>8),(unsigned char)(v>>16)};
        fwrite(b,1,3,audio);
    }
    dsp_ram[slot]=0;
    dsp_ram[CTRL+1]=seq+1>=SEQ_WRAP ? 0 : seq+1;
    dsp_ram[CTRL_READ_SLOT]=slot+SLOT_WORDS>=SLOT0+SLOT_COUNT*SLOT_WORDS ? SLOT0 : slot+SLOT_WORDS;
    (*blocks)++;
    return 1;
}
int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"--layout")) {
        printf("{\"state\":%zu,\"drum_pool\":%u,\"drum_voice\":%zu,\"drum_active\":%zu,\"requested\":%zu,\"served\":%zu,"
               "\"seed\":%zu,\"generated\":%zu,\"audition_track\":%zu,\"last_family\":%zu,\"pad_events\":%zu,"
               "\"plock_locks\":%zu,\"spices\":%zu,\"gfx_owner\":%zu,\"path\":%zu,\"report_bytes\":%u}\n",
               sizeof(State),DRUM_POOL,sizeof(PhVoice),offsetof(PhVoice,active),offsetof(State,requested),offsetof(State,served),
               offsetof(State,seed),offsetof(State,generated),offsetof(State,audition_track),offsetof(State,last_family),
               offsetof(State,pad_events),offsetof(State,plock_locks),offsetof(State,spices),offsetof(State,gfx_owner),offsetof(State,path),REPORT_BYTES);
        return 0;
    }
    if(argc==4 && !strcmp(argv[1],"--store")) {
        /* Shift + track f, audition to its end, then store into slot 19 */
        unsigned family=(unsigned)strtoul(argv[2],0,0);
        boot();
        fm_randomize_request(family,0);
        unsigned blocks=0;
        FILE *null=fopen("/dev/null","wb");
        for(unsigned call=0;call<6000u && (D[0].active || call<2);call++) { fm_tick(); while(consume(null,0,&blocks)>0); }
        fclose(null);
        if(!commit_start(family,19) || S->commits!=1 || host_reloads!=1 || host_auditions!=1) return 6;
        FILE *wav=fopen(argv[3],"wb"); fwrite(host_files[19],1,host_lens[19],wav); fclose(wav);
        printf("stored family %u samples %u bytes %u\n",family,S->pending[family].samples,host_lens[19]);
        return 0;
    }
    if(argc==3 && !strcmp(argv[1],"--kit")) {
        /* a whole kit grown page by page (8 x 16 = 128 sounds), each page
           planned from the tags of the pages before; then page 4 cleared.
           Each sound written as <dir>/slotNNN.wav; one line per sound. */
        static const char *cat[C_COUNT]={"kick","kick-g","snare","snare-g","clap","impact","hat","cymbal","metal","metal-g","noise","click","destroy"};
        boot();
        unsigned counts[C_COUNT]={0};
        for(unsigned page=0;page<8u;page++) {
            S->kit_start=16u*page; kit_make();
            if(S->kit_state || S->kits!=page+1u || S->commits!=16u*(page+1u)) return 7;
            if(S->kit_known!=16u*page) return 10;
            printf("focus page %u: %08x\n",page+1u,S->kit_focus);
            for(unsigned i=0;i<16u;i++) {
                unsigned slot=16u*page+i; char fn[512]; snprintf(fn,sizeof fn,"%s/slot%03u.wav",argv[2],slot+1u);
                FILE *wav=fopen(fn,"wb"); fwrite(host_files[slot],1,host_lens[slot],wav); fclose(wav);
                const uint8_t *h=host_files[slot];
                if(memcmp(h+38,"PHK1",4)) return 11;
                const int16_t *x=(const int16_t *)(h+64); unsigned n=(host_lens[slot]-64u)/2u, zc=0;
                for(unsigned k=1;k<n;k++) if((x[k-1]<0)!=(x[k]<0)) zc++;
                unsigned c=kit_category(h[42],h[44]); counts[c]++;
                printf("page %u slot %3u %-8s pad %2u arche %u kind %2u samples %6u zc_per_s %6u\n",page+1u,slot+1u,cat[c],h[42],h[43],h[45],n-256u,n ? (unsigned)((uint64_t)zc*48000u/n) : 0u);
            }
        }
        printf("whole kit:");
        for(unsigned c=0;c<C_COUNT;c++) printf(" %s %u",cat[c],counts[c]);
        printf("\n");
        S->kit_start=48; kit_clear();
        for(unsigned i=0;i<16u;i++) if(host_lens[48u+i]) return 8;
        printf("cleared page 4, deletes %u, reloads %u, tag reads %u\n",host_deletes,host_reloads,host_reads);
        return S->clears==1 && S->kit_state==0 ? 0 : 9;
    }
    if(argc!=3) return 2;
    boot();
    if(S->magic!=MAGIC || *HEAP_LIMIT!=0x2004f760) return 3;
    FILE *audio=fopen(argv[1],"wb"),*first=fopen(argv[2],"wb");
    if(!audio || !first) return 2;
    unsigned blocks=0;
    for(unsigned family=0;family<PH_FAMILIES;family++) {
        unsigned before=blocks, idle=0;
        fm_randomize_request(family,0);
        /* run until the audition ended (several idle service calls), at most 4 s */
        for(unsigned call=0;call<6000u && idle<8u;call++) {
            fm_tick();
            int got=0, r;
            while((r=consume(audio,family==0 ? first:0,&blocks))>0) got=1;
            if(r<0) return 4;
            idle=got ? 0 : idle+1;
        }
        if(D[0].active) return 5;
        printf("family %u pad %u kind %u blocks %u peak %u\n",family,S->last_pad,S->last_kind,blocks-before,S->peak);
        S->peak=0;
    }
    fclose(first); fclose(audio);
    printf("%u blocks, %zu bytes MCU state\n",blocks,sizeof(State));
    return 0;
}
