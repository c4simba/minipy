/* ========================= Typed compiler, code generator: the program =========================
   Classes' vtables, literal pools, dynamic tables, and the whole
   listing (aot_generate, aot_compile). */

#include "gen.h"
#include "targets/targets.h"

static void emit_class(AClass *c){
    const char *mn= c->builtin || !c->mod || !c->mod->name ? "builtins" : c->mod->name;     /* (before it: its module's name, and "module." as */
    char pre[300]; snprintf(pre,sizeof pre,"%s.",mn);                                       /* an uncaught exception shows it: none for __main__'s and built-in ones) */
    if(!strcmp(mn,"builtins") || !strcmp(mn,"__main__")) pre[0]=0;
    char gcm[40]="";                                                                        /* the cycle collector's map (vtable-12; 0: none) */
    if(gg->gc_on) snprintf(gcm,sizeof gcm,"        dd %s\n",gg->gc_cls[c->id]?gc_class_map(c):"0");
    buf_printf(&gg->data,"align 4\n%s        dd S%d,S%d\n%s dd S%d,",gcm,str_lit(mn),str_lit(pre),class_label(c,0),str_lit(c->name));
    if(c->base) buf_printf(&gg->data,"%s",class_label(c->base,0)); else buf_printf(&gg->data,"0");
    for(int i=0;i<c->nvt;i++) buf_printf(&gg->data,",%s",c->vt[i]->unused||!c->vt[i]->used?"0":fnl(c->vt[i],LB_CODE));   /* (a mixin's own methods: only its copies run; uncalled ones: none) */
    buf_printf(&gg->data,"   ; class %s\n",c->name);
    Buf *o=&gg->text; int any=0;
    buf_printf(o,"\n%s:                          ; destroy %s\n",class_label(c,1),c->name);
    for(AClass *k=c;k;k=k->base) for(int i=0;i<k->nfields;i++) if(is_ptr(k->fields[i]->ty) && !k->fields[i]->over){
        if(!any){ buf_printf(o,"        push ebx\n        mov ebx,eax\n"); any=1; }
        buf_printf(o,"        mov eax,[ebx+%d]\n        call rt_decref\n",k->fields[i]->offset);
    }
    if(any) buf_printf(o,"        mov eax,ebx\n        pop ebx\n");
    buf_printf(o,"        jmp rt_free\n");
}

/* ---------------------------------------------------------------- the program */

static void emit_lit_pools(void){
    Buf *d=&gg->data;
    for(int i=0;i<gg->ntdescs;i++){ Ty *t=gg->tdescs[i];
        buf_printf(d,"align 4\nTD%d dd %d",i,t->nelems);
        buf_printf(d,",%d",tuple_size(t));
        for(int k=0;k<t->nelems;k++) buf_printf(d,",%s,%d",kd_of(t->elems[k]),tuple_off(t,k));
        buf_printf(d,"   ; %s\n",ty_name(t)); }
    if(gg->nflts){ buf_printf(d,"align 8\n");            /* (as bits: fasm does not always round a decimal one right) */
        for(int i=0;i<gg->nflts;i++){ double v=strtod(gg->flts[i].s,NULL); unsigned long long b; memcpy(&b,&v,8);
            buf_printf(d,"FC%d dd 0x%08X,0x%08X ; %s\n",i,(unsigned)(b&0xFFFFFFFFu),(unsigned)(b>>32),gg->flts[i].s); } }
    for(int i=0;i<gg->nlits;i++){
        buf_printf(d,"align 4\nS%d dd 0x40000000,rt_static,%d\n        ",i,gg->lits[i].len);
        buf_bytes(d,gg->lits[i].s,gg->lits[i].len,1);
        buf_printf(d,"        dd %d\n",u8_cplen(gg->lits[i].s,gg->lits[i].len));
    }
    for(int i=0;i<gg->nblits;i++){                               /* bytes: one code point per byte */
        buf_printf(d,"align 4\nB%d dd 0x40000000,rt_static,%d\n        ",i,gg->blits[i].len);
        buf_bytes(d,gg->blits[i].s,gg->blits[i].len,1);
        buf_printf(d,"        dd %d\n",gg->blits[i].len);
    }
    for(int i=0;i<gg->nplits;i++){ buf_printf(d,"P%d ",i); Lit *l=&gg->plits[i]; char *t=(char*)xmalloc((size_t)l->len+1); memcpy(t,l->s,(size_t)l->len); t[l->len]='\n'; buf_bytes(d,t,l->len+1,0); free(t); }
    for(int i=0;i<gg->nzlits;i++){ buf_printf(d,"Z%d ",i); buf_bytes(d,gg->zlits[i].s,gg->zlits[i].len,1); }
    for(int i=0;i<gg->nrlits;i++){ buf_printf(d,"RL%d ",i); buf_bytes(d,gg->rlits[i].s,gg->rlits[i].len,0); }
}

/* Dynamic linking (ctypes): the C functions the program calls are imported
   from their libraries by ld-linux.so.2 - a PT_INTERP segment, a PT_DYNAMIC
   one, and symbol / relocation / hash / string tables for the slots CI<id>
   (R_386_32: the linker writes each function's address there at start). */
static int dyn_used(AProg *p){ for(int i=0;i<p->ncfuncs;i++) if(p->cfuncs[i]->used) return 1; return 0; }
static void emit_dynamic_header(Buf *o, AProg *p){
    buf_printf(o,"segment interpreter readable\n        db '/lib/ld-linux.so.2',0\n\nsegment dynamic readable\n");
    for(int i=0;i<p->nclibs;i++) if(p->clibs[i]->used) buf_printf(o,"        dd 1,DYN_L%d-DYN_strtab       ; DT_NEEDED %s\n",i,p->clibs[i]->soname);
    buf_printf(o,"        dd 5,DYN_strtab,10,DYN_strsz   ; DT_STRTAB, DT_STRSZ\n"
                 "        dd 6,DYN_symtab,11,16          ; DT_SYMTAB, DT_SYMENT\n"
                 "        dd 17,DYN_rel,18,DYN_relsz,19,8   ; DT_REL, DT_RELSZ, DT_RELENT\n"
                 "        dd 4,DYN_hash                  ; DT_HASH\n"
                 "        dd 0,0\n\n");
}
static void emit_dynamic_tables(Buf *o, AProg *p){
    int n=0;
    buf_printf(o,"\n; ---- imports of the dynamic linker\nalign 4\nDYN_symtab:\n        dd 0,0,0,0\n");
    for(int i=0;i<p->ncfuncs;i++) if(p->cfuncs[i]->used){ n++; buf_printf(o,"        dd DYN_S%d-DYN_strtab,0,0\n        db 0x12,0\n        dw 0\n",i); }   /* STB_GLOBAL, STT_FUNC */
    buf_printf(o,"DYN_rel:\n");
    int k=0;
    for(int i=0;i<p->ncfuncs;i++) if(p->cfuncs[i]->used){ k++; buf_printf(o,"        dd CI%d,(%d shl 8) or 1   ; R_386_32 %s\n",i,k,p->cfuncs[i]->sym); }
    buf_printf(o,"DYN_relsz = $-DYN_rel\nDYN_hash:\n        dd 1,%d,0\n",n+1);           /* one bucket, empty: nothing to export */
    for(int i=0;i<=n;i++) buf_printf(o,"        dd 0\n");
    buf_printf(o,"DYN_strtab db 0\n");
    for(int i=0;i<p->ncfuncs;i++) if(p->cfuncs[i]->used) buf_printf(o,"DYN_S%d db '%s',0\n",i,p->cfuncs[i]->sym);
    for(int i=0;i<p->nclibs;i++) if(p->clibs[i]->used) buf_printf(o,"DYN_L%d db '%s',0\n",i,p->clibs[i]->soname);
    buf_printf(o,"DYN_strsz = $-DYN_strtab\nalign 4\n");
    for(int i=0;i<p->ncfuncs;i++) if(p->cfuncs[i]->used){
        buf_printf(o,"CI%d dd 0   ; %s\n",i,p->cfuncs[i]->sym);
        if(!strcmp(p->cfuncs[i]->sym,"fflush")) buf_printf(o,"CI_fflush = CI%d\n",i);
    }
}
/* macos: no dynamic linker - the C translation looks the functions up when
   the program starts; ";@import CI<id> <library> <symbol>" names each slot. */
static void emit_import_slots(Buf *o, AProg *p){
    buf_printf(o,"\n; ---- C functions (ctypes)\nalign 4\n");
    for(int i=0;i<p->ncfuncs;i++) if(p->cfuncs[i]->used){
        buf_printf(o,";@import CI%d %s %s\nCI%d dd 0   ; %s\n",i,p->cfuncs[i]->lib->soname,p->cfuncs[i]->sym,i,p->cfuncs[i]->sym);
        if(!strcmp(p->cfuncs[i]->sym,"fflush")) buf_printf(o,"CI_fflush = CI%d\n",i);
    }
}
/* With the C library linked, its stdio buffers are flushed at exit (fflush(NULL)). */
static void import_fflush(AProg *p){
    ACLib *libc=NULL;
    for(int i=0;i<p->nclibs;i++) if(p->clibs[i]->used && !strcmp(p->clibs[i]->soname,"libc.so.6")) libc=p->clibs[i];
    if(!libc) return;
    for(int i=0;i<p->ncfuncs;i++) if(p->cfuncs[i]->lib==libc && !strcmp(p->cfuncs[i]->sym,"fflush")){ p->cfuncs[i]->used=1; return; }
    ACFunc *f=PPY_NEW0(ACFunc); f->lib=libc; f->sym=xstrdup2("fflush"); f->id=p->ncfuncs; f->used=1; f->nargtypes=-1;
    p->cfuncs=(ACFunc**)xrealloc(p->cfuncs,sizeof(ACFunc*)*(size_t)(p->ncfuncs+1)); p->cfuncs[p->ncfuncs++]=f;
}

/* x87 rounding to double precision: float arithmetic gives Python's results */
#define FPU_DOUBLE "        push 0x027F\n        fldcw [esp]\n        pop eax\n"

int aot_generate(AProg *p, const AotCodegenOptions *opt, char **out, size_t *outlen){
    G g; memset(&g,0,sizeof g); gg=&g;
    const AotTargetDef *tg=aot_target(opt->target);
    g.p=p; g.target=opt->target; g.stack=opt->stack_size?opt->stack_size:tg->stack?tg->stack:65536;
    g_labels.n=0; for(int k=0;k<LB_KINDS;k++) g_fnlabels[k]=NULL;          /* a fresh set of label names */
    g_vtlabels=g_dtlabels=g_glabels=g_mlabels=NULL;
    g.rt=aot_rt_new(opt->target);
    g.class_used=(int*)xmalloc(sizeof(int)*(size_t)(p->nclasses+1)); memset(g.class_used,0,sizeof(int)*(size_t)(p->nclasses+1));
    g.fv_used=(char*)xmalloc((size_t)p->nfuncs+1); memset(g.fv_used,0,(size_t)p->nfuncs+1);
    g.cd_used=(char*)xmalloc((size_t)p->nfuncs+1); memset(g.cd_used,0,(size_t)p->nfuncs+1);
    g.bm_used=(char*)xmalloc((size_t)p->nfuncs+1); memset(g.bm_used,0,(size_t)p->nfuncs+1);
    AFunc *main_body=p->mods[0]->body;
    use_fn(main_body);
    if(p->uncaught_fn) use_fn(p->uncaught_fn);
    rt("rt_exit");
    if(!getenv("PPY_NO_CONSUME")) for(int i=0;i<p->nfuncs;i++) if(!p->funcs[i]->unused) plan_consumed(p->funcs[i]);
    gc_analyze();                                              /* which objects can be in reference cycles */
    aot_peephole_reset();                                      /* the functions whose entry reads edx (their closure): see emit_function */
    for(int i=0;i<p->nfuncs;i++){ AFunc *fn=p->funcs[i]; if(!fn->unused && (fn->ncaps || (fn->outer && dyn_defaults(fn)))) aot_peephole_takes_edx(fnl(fn,LB_CODE)); }
    int nf=0, ns=0, na=0;
    for(;;){                                   /* functions, formatters, task entries and adapters pull in more of each other */
        int progress=0;
        for(int i=0;i<g.nq;i++) if(g.queue[i]){ AFunc *fn=g.queue[i]; g.queue[i]=NULL; emit_function(fn); progress=1; }
        for(;nf<g.nfmts;nf++){ emit_formatter(nf); progress=1; }
        for(;ns<g.nstubs;ns++){ emit_task_stub(ns); progress=1; }
        for(;na<g.nadapters;na++){ emit_adapter(na); progress=1; }
        if(!progress) break;
    }
    for(int i=0;i<p->nfuncs;i++) if(g.bm_used[i]) emit_bound_method(p->funcs[i]);
    if(g.nadapters || memchr(g.bm_used,1,(size_t)p->nfuncs)){ rt("rt_decref"); rt("rt_free"); }
    for(int i=0;i<g.nnas;i++) buf_printf(&g.text,"%sNA%d:   mov esi,Z%d\n        jmp rt_attrerr_none\n",i?"":"\n; AttributeError: 'NoneType' object has no attribute <name>\n",g.nas[i],g.nas[i]);
    for(int i=0;i<p->nfuncs;i++){
        if(g.cd_used[i]) emit_closure_destroy(p->funcs[i]);
        if(g.cd_used[i] && g.gc_clo[p->funcs[i]->id]) gc_closure_map(p->funcs[i]);
        if(g.fv_used[i]){ AFunc *fi=p->funcs[i]; layout_defaults(fi);
            buf_printf(&g.data,"align 4\n%s dd 0x40000000,rt_static,%s,S%d   ; %s as a value\n",fnl(fi,LB_VALUE),fi->used?fnl(fi,LB_CODE):"0",str_lit(fn_display_name(fi)),fn_display_name(fi));   /* (0: only its defaults are used) */
            if(fi->defsize){ buf_printf(&g.data,"        dd 0"); for(int k=4;k<fi->defsize;k+=4) buf_printf(&g.data,",0"); buf_printf(&g.data,"   ; its defaults\n"); } }
    }
    if(aot_rt_used(g.rt,"rt_throw")){                   /* run-time errors raise built-in exceptions */
        static const char *rtexc[]={"IndexError","KeyError","ZeroDivisionError","AttributeError","ValueError","AssertionError","FileNotFoundError","StopIteration","TypeError","OverflowError","EOFError","LookupError","UnicodeDecodeError","UnicodeEncodeError","UnboundLocalError","NameError","GeneratorExit","RuntimeError",NULL};
        rt("rt_raise_builtin");
        if(aot_rt_used(g.rt,"rt_gen_new")) rt("rt_gen_close");          /* abandoned generators are closed (their finally blocks run) */
        { int mx=0; for(int i=0;i<p->nclasses;i++) if(p->classes[i]->builtin && aot_is_exception(p->classes[i]) && p->classes[i]->size>mx) mx=p->classes[i]->size;
          buf_printf(&g.data,"RT_EXC_SIZE = %d\n",mx); }                  /* (the largest built-in one: an OSError's fields) */
        for(int k=0;rtexc[k];k++) for(int i=0;i<p->nclasses;i++) if(p->classes[i]->builtin && !strcmp(p->classes[i]->name,rtexc[k])){
            use_class(p->classes[i]);
            buf_printf(&g.data,"VTX_%s = %s\nDTX_%s = %s\n",rtexc[k],class_label(p->classes[i],0),rtexc[k],class_label(p->classes[i],1));
        }
    }
    for(int i=0;i<p->nclasses;i++) if(g.class_used[i]) emit_class(p->classes[i]);
    for(int i=0;i<g.nbtypes;i++)                                 /* built-in types as values: name, base */
        buf_printf(&g.data,"align 4\n        dd S%d,S%d\nTYD_%s dd S%d,%s\n",str_lit("builtins"),str_lit(""),g.btypes[i],str_lit(g.btypes[i]),!strcmp(g.btypes[i],"bool")?"TYD_int":"0");
    if(aot_rt_used(g.rt,"rt_sb_type")){                          /* "<class '__main__.C'>": qualified names */
        buf_printf(&g.data,"align 4\nTYQ");
        for(int i=0;i<p->nclasses;i++) if(g.class_used[i] && !p->classes[i]->builtin){ AClass *c=p->classes[i]; char q[300];
            snprintf(q,sizeof q,"%s.%s",c->mod->name,c->qualname); buf_printf(&g.data," dd %s,S%d\n   ",class_label(c,0),str_lit(q)); }
        buf_printf(&g.data," dd 0\n"); }
    if(aot_rt_used(g.rt,"rt_type_qualname")){                    /* __qualname__ unlike __name__ */
        buf_printf(&g.data,"align 4\nTYQN");
        for(int i=0;i<p->nclasses;i++) if(g.class_used[i] && strcmp(p->classes[i]->qualname,p->classes[i]->name)){ AClass *c=p->classes[i];
            buf_printf(&g.data," dd %s,S%d\n   ",class_label(c,0),str_lit(c->qualname)); }
        buf_printf(&g.data," dd 0\n"); }
    if(g.gc_on){                                                 /* the cycle collector (aot/runtime, rt_gc_root) */
        rt("rt_gc_root");
        buf_printf(&g.data,"RT_GC = 1\nalign 4\nrt_gc_table dd %d   ; (destroy routine, map of references) of objects that can be in a cycle\n",g.gc_ntab);
        buf_cat(&g.data,&g.gc_tab); free(g.gc_tab.s); }
    emit_lit_pools();
    for(int i=0;i<p->nglobals;i++){ AVar *v=p->globals[i]; buf_printf(&g.bss,"%s rd %d   ; %s.%s\n",global_label(v),esize(v->ty)/4,v->mod->name,v->name); }
    for(int i=1;i<p->nmods;i++) if(p->mods[i]->used) buf_printf(&g.bss,"%s rb 1   ; %s imported\n",module_label(p->mods[i]),p->mods[i]->name);

    { size_t ol; char *ot=aot_outline(g.text.s?g.text.s:"",g.text.len,&ol);     /* repeated call sequences shared (aot/peep.c) */
      free(g.text.s); g.text.s=ot; g.text.len=ol; g.text.cap=ol+1; }
    Buf o; memset(&o,0,sizeof o);
    int heap=aot_rt_used(g.rt,"rt_os_alloc")||aot_rt_used(g.rt,"rt_con_open");
    buf_printf(&o,"; generated by plainpy --compile from %s\n",p->mods[0]->unit->path?p->mods[0]->unit->path:"?");
    char hook[200]="";                                     /* an uncaught exception: _ppy_exit.__ppy_uncaught (SystemExit's status) */
    if(p->uncaught_fn && aot_rt_used(g.rt,"rt_exc_uncaught")) snprintf(hook,sizeof hook,"        mov dword [rt_uncaught_hook],%s\n",fnl(p->uncaught_fn,LB_CODE));
    if(g.gc_on && getenv("PPY_GC_STRESS")){ size_t l=strlen(hook); snprintf(hook+l,sizeof hook-l,"        mov dword [rt_gc_limit],1\n"); buf_printf(&g.data,"RT_GC_STRESS = 1\n"); }   /* (a test of the cycle collector) */
    const char *report="";
    if(opt->count_allocs && aot_rt_used(g.rt,"rt_alloc")){ rt("rt_live_report"); buf_printf(&g.data,"RT_COUNT_ALLOCS = 1\n");
        report=g.gc_on?"        call rt_gc_collect\n        call rt_live_report\n":"        call rt_live_report\n"; }   /* (cycles left at the end: collected first) */
    int kolibri=tg->abi==AOT_ABI_KOLIBRI, dyn=tg->cfn!=AOT_CFN_NONE && dyn_used(p);
    if(dyn && tg->cfn==AOT_CFN_DYNAMIC) import_fflush(p);
    {   unsigned chunk=opt->chunk?opt->chunk:tg->chunk, costack=opt->costack?opt->costack:tg->costack;   /* (memory: aot/runtime memory.asm, generator.asm, asyncio.asm) */
        buf_printf(&o,"; target %s\n",tg->name);
        buf_printf(&g.data,"RT_CHUNK = %u     ; bytes the allocator takes from the system at a time\nRT_COSTACK = %u   ; the stack of a generator or a task\n",
                   (chunk+4095u)&~4095u,(costack+4095u)&~4095u); }
    if(!kolibri){
        if(tg->out==AOT_OUT_C) buf_printf(&o,"; plainpy translates this listing to C (aot/x2c.c)\n");
        buf_printf(&o,"format ELF executable 3\nentry start\n\n");
        if(dyn && tg->out==AOT_OUT_FASM) emit_dynamic_header(&o,p);
        buf_printf(&o,"segment readable executable\n\nstart:\n%s        fninit\n%s%s",aot_rt_used(g.rt,"rt_sys_args")?"        mov [rt_sp0],esp\n":"",FPU_DOUBLE,hook);
        buf_printf(&o,"        call %s\n%s%s        xor ebx,ebx\n        jmp rt_exit\n",fnl(main_body,LB_CODE),aot_rt_used(g.rt,"rt_thread_drain")?"        call rt_thread_drain\n":"",report);
    } else {
        buf_printf(&o,"format binary as ''\nuse32\n        org 0\n        db 'MENUET01'\n        dd 1,start,i_end,mem_end,stack_top,%s\n\nstart:\n        fninit\n%s",
                   aot_rt_used(g.rt,"rt_sys_args")?"rt_kparams,rt_kpath":"0,0",FPU_DOUBLE);
        if(heap) buf_printf(&o,"        mov eax,68\n        mov ebx,11\n        int 0x40\n");
        buf_printf(&o,"%s",hook);
        buf_printf(&o,"        call %s\n%s%s        xor ebx,ebx\n        jmp rt_exit\n",fnl(main_body,LB_CODE),aot_rt_used(g.rt,"rt_thread_drain")?"        call rt_thread_drain\n":"",report);
    }
    buf_cat(&o,&g.text);
    buf_printf(&o,"\n; ---- runtime\n");
    aot_rt_emit(g.rt,0,put_cb,&o);
    if(!kolibri) buf_printf(&o,"\nsegment readable writeable\n\n");
    else buf_printf(&o,"\n; ---- data\n");
    if(g.gc_on) buf_printf(&o,"        dd 0                    ; (rt_decref reads the dword before an object: a static one too)\n");
    if(dyn) (tg->out==AOT_OUT_C ? emit_import_slots : emit_dynamic_tables)(&o,p);
    buf_cat(&o,&g.data);
    aot_rt_emit(g.rt,1,put_cb,&o);
    if(kolibri) buf_printf(&o,"\nalign 4\ni_end:\n");
    buf_printf(&o,"\n; ---- uninitialized\nalign 8\n");
    buf_cat(&o,&g.bss);
    aot_rt_emit(g.rt,2,put_cb,&o);
    if(kolibri) buf_printf(&o,"align 16\n        rb %u\nstack_top:\nmem_end:\n",(g.stack+15)&~15u);
    *out=o.s; *outlen=o.len;
    gg=NULL;
    return 0;
}

int aot_compile(const AotCodegenOptions *opt, AotUnit **units, int nunits, char **out, size_t *outlen){
    AProg *p=aot_check(units,nunits,opt->target);
    if(!p) return 1;
    int rc=aot_generate(p,opt,out,outlen);
    aot_peephole_report();
    return rc;
}
