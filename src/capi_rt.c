/* ========================= cpython target: runtime =========================
   See capi_rt.h. Written into the build directory of every program compiled
   with --target cpython and built with it. */
#include "mpy_rt.h"
#include <string.h>
#include <stddef.h>

static PyObject *mpy_builtins;          /* the builtins module's dict */
uint64_t mpy_gver=1;                    /* changes whenever a module dict or the builtins change */
static PyObject *mpy_stubs;             /* the trampolines' code objects (a set): left out of tracebacks */
static vectorcallfunc func_vectorcall;  /* CPython's own vectorcall of functions */
/* a compiled function called with just its positional parameters: its C, no trampoline frame */
static PyObject *func_vc(PyObject *f, PyObject *const *args, size_t nargsf, PyObject *kwnames){
    if(!kwnames){
        PyObject *clo=PyFunction_GET_CLOSURE(f);
        PyObject *e= clo && PyTuple_GET_SIZE(clo)==1 ? PyCell_GET(PyTuple_GET_ITEM(clo,0)) : NULL;
        if(e && Py_IS_TYPE(e,&MpyEnv_Type) && ((MpyEnv*)e)->direct==(Py_ssize_t)PyVectorcall_NARGS(nargsf)){
            if(Py_EnterRecursiveCall(" in compiled code")) return NULL;
            PyObject *r=((MpyEnv*)e)->impl((MpyEnv*)e,args);
            Py_LeaveRecursiveCall();
            return r;
        }
    }
    return func_vectorcall(f,args,nargsf,kwnames);
}
/* the traceback without the trampolines' entries */
static PyObject *strip_tb(PyObject *tb){
    if(!tb || tb==Py_None || !mpy_stubs) return tb;
    PyTracebackObject *t=(PyTracebackObject*)tb;
    PyObject *next=strip_tb(t->tb_next?(PyObject*)t->tb_next:NULL);
    if(next!=(PyObject*)t->tb_next){ Py_XSETREF(t->tb_next,(PyTracebackObject*)Py_XNewRef(next)); }
    PyCodeObject *c=PyFrame_GetCode(t->tb_frame);
    int stub=PySet_Contains(mpy_stubs,(PyObject*)c)>0;
    Py_DECREF(c);
    return stub ? next : tb;
}
static void strip_exc(PyObject *exc){
    PyObject *tb=PyException_GetTraceback(exc);
    if(!tb) return;
    PyObject *s=strip_tb(tb);
    if(s!=tb) PyException_SetTraceback(exc,s?s:Py_None);
    Py_DECREF(tb);
}
static int mpy_watcher=-1;
static int watch_cb(PyDict_WatchEvent ev, PyObject *d, PyObject *k, PyObject *v){ (void)ev; (void)d; (void)k; (void)v; mpy_gver++; return 0; }
static int watch(PyObject *d){
    if(mpy_watcher<0){ mpy_watcher=PyDict_AddWatcher(watch_cb); if(mpy_watcher<0) return -1; }
    return PyDict_Watch(mpy_watcher,d);
}

/* ---------------------------------------------------------------- MpyEnv */
static PyObject *env_vectorcall(PyObject *self, PyObject *const *args, size_t nargsf, PyObject *kwnames){
    (void)nargsf; (void)kwnames;
    MpyEnv *e=(MpyEnv*)self;
    return e->impl(e,args);
}
static int env_traverse(PyObject *self, visitproc visit, void *arg){ MpyEnv *e=(MpyEnv*)self; Py_VISIT(e->cells); Py_VISIT(e->globals); return 0; }
static int env_clear(PyObject *self){ MpyEnv *e=(MpyEnv*)self; Py_CLEAR(e->cells); Py_CLEAR(e->globals); return 0; }
static void env_dealloc(PyObject *self){ PyObject_GC_UnTrack(self); env_clear(self); PyObject_GC_Del(self); }
PyTypeObject MpyEnv_Type={
    PyVarObject_HEAD_INIT(NULL,0)
    .tp_name="minipy.env",
    .tp_basicsize=sizeof(MpyEnv),
    .tp_flags=Py_TPFLAGS_DEFAULT|Py_TPFLAGS_HAVE_GC|Py_TPFLAGS_HAVE_VECTORCALL,
    .tp_vectorcall_offset=offsetof(MpyEnv,vectorcall),
    .tp_call=PyVectorcall_Call,
    .tp_dealloc=env_dealloc,
    .tp_traverse=env_traverse,
    .tp_clear=env_clear,
};
static PyObject *env_new(MpyImpl impl, PyObject *cells, PyObject *globals){
    MpyEnv *e=PyObject_GC_New(MpyEnv,&MpyEnv_Type);
    if(!e) return NULL;
    e->vectorcall=env_vectorcall; e->impl=impl; e->cells=Py_XNewRef(cells); e->globals=Py_NewRef(globals); e->direct=-1;
    PyObject_GC_Track(e);
    return (PyObject*)e;
}

/* ---------------------------------------------------------------- code objects */
static int index_codes(PyObject *code, PyObject *list){
    if(PyList_Append(list,code)) return -1;
    PyObject *consts=PyObject_GetAttrString(code,"co_consts");
    if(!consts) return -1;
    for(Py_ssize_t i=0;i<PyTuple_GET_SIZE(consts);i++){
        PyObject *c=PyTuple_GET_ITEM(consts,i);
        if(PyCode_Check(c) && index_codes(c,list)){ Py_DECREF(consts); return -1; }
    }
    Py_DECREF(consts);
    return 0;
}
static int code_str_is(PyObject *code, const char *attr, const char *s){
    PyObject *v=PyObject_GetAttrString(code,attr);
    if(!v){ PyErr_Clear(); return 0; }
    int r=PyUnicode_Check(v) && PyUnicode_CompareWithASCIIString(v,s)==0;
    Py_DECREF(v); return r;
}
static int code_line(PyObject *code){
    PyObject *v=PyObject_GetAttrString(code,"co_firstlineno");
    if(!v){ PyErr_Clear(); return -1; }
    int r=(int)PyLong_AsLong(v); Py_DECREF(v); return r;
}
int mpy_load_codes(const unsigned char *code, Py_ssize_t codelen, const unsigned char *stub, Py_ssize_t stublen,
                   const MpyCodeRef *refs, int n, PyObject **out, const char *modname){
    PyObject *mc=NULL, *sc=NULL, *mlist=PyList_New(0), *slist=PyList_New(0);
    int rc=-1;
    if(!mlist || !slist) goto done;
    if(codelen){ mc=PyMarshal_ReadObjectFromString((const char*)code,codelen); if(!mc || index_codes(mc,mlist)) goto done; }
    if(stublen){ sc=PyMarshal_ReadObjectFromString((const char*)stub,stublen); if(!sc || index_codes(sc,slist)) goto done; }
    for(int i=0;i<n;i++){
        const MpyCodeRef *r=&refs[i]; PyObject *found=NULL;
        if(r->stub){
            for(Py_ssize_t k=0;k<PyList_GET_SIZE(slist) && !found;k++){ PyObject *c=PyList_GET_ITEM(slist,k); if(code_str_is(c,"co_name",r->name)) found=c; }
            if(!found){ PyErr_Format(PyExc_SystemError,"minipy: %s: trampoline %s is missing",modname,r->name); goto done; }
            PyObject *rep=PyObject_GetAttrString(found,"replace"), *kw=Py_BuildValue("{s:s,s:s,s:i}","co_name",r->rename,"co_qualname",r->qualname,"co_firstlineno",r->line);
            PyObject *e=PyTuple_New(0);
            out[i]= rep&&kw&&e ? PyObject_Call(rep,e,kw) : NULL;
            Py_XDECREF(rep); Py_XDECREF(kw); Py_XDECREF(e);
            if(!out[i]) goto done;
            if(!mpy_stubs && !(mpy_stubs=PySet_New(NULL))) goto done;
            if(PySet_Add(mpy_stubs,out[i])) goto done;
        } else {
            int seen=0;
            for(Py_ssize_t k=0;k<PyList_GET_SIZE(mlist) && !found;k++){
                PyObject *c=PyList_GET_ITEM(mlist,k);
                if(code_line(c)==r->line && code_str_is(c,"co_qualname",r->name)){ if(seen==r->nth) found=c; seen++; }
            }
            if(!found){ PyErr_Format(PyExc_SystemError,"minipy: %s: no code object %s at line %d",modname,r->name,r->line); goto done; }
            out[i]=Py_NewRef(found);
        }
    }
    rc=0;
done:
    Py_XDECREF(mc); Py_XDECREF(sc); Py_XDECREF(mlist); Py_XDECREF(slist);
    return rc;
}

/* ---------------------------------------------------------------- functions and classes */
static int set_func_attrs(PyObject *f, PyObject *defaults, PyObject *kwdefaults, PyObject *annotate, PyObject *annotations){
    if(defaults && PyFunction_SetDefaults(f,defaults)) return -1;
    if(kwdefaults && PyFunction_SetKwDefaults(f,kwdefaults)) return -1;
    if(annotate && PyObject_SetAttrString(f,"__annotate__",annotate)) return -1;
    if(annotations && PyFunction_SetAnnotations(f,annotations)) return -1;
    return 0;
}
PyObject *mpy_func(PyObject *code, PyObject *globals, MpyImpl impl, PyObject *cells, PyObject *defaults, PyObject *kwdefaults,
                   PyObject *annotate, PyObject *annotations, PyObject *doc, Py_ssize_t direct){
    PyObject *env=env_new(impl,cells,globals);
    if(!env) return NULL;
    ((MpyEnv*)env)->direct=direct;
    PyObject *f=PyFunction_New(code,globals);
    if(!f){ Py_DECREF(env); return NULL; }
    PyObject *cell=PyCell_New(env); Py_DECREF(env);
    PyObject *clo=cell?PyTuple_Pack(1,cell):NULL; Py_XDECREF(cell);
    if(!clo || PyFunction_SetClosure(f,clo)){ Py_XDECREF(clo); Py_DECREF(f); return NULL; }
    Py_DECREF(clo);
    if(set_func_attrs(f,defaults,kwdefaults,annotate,annotations) || PyObject_SetAttrString(f,"__doc__",doc?doc:Py_None)){ Py_DECREF(f); return NULL; }
    if(direct>=0){
        if(!func_vectorcall) func_vectorcall=((PyFunctionObject*)f)->vectorcall;
        PyFunction_SetVectorcall((PyFunctionObject*)f,func_vc);
    }
    return f;
}
static PyObject *closure_for(PyObject *code, PyObject *const *cellv, PyObject *const *cellnames, int ncells){
    PyObject *fv=PyCode_GetFreevars((PyCodeObject*)code);
    if(!fv) return NULL;
    Py_ssize_t n=PyTuple_GET_SIZE(fv);
    if(!n){ Py_DECREF(fv); return Py_NewRef(Py_None); }
    PyObject *clo=PyTuple_New(n);
    if(!clo){ Py_DECREF(fv); return NULL; }
    for(Py_ssize_t i=0;i<n;i++){
        PyObject *name=PyTuple_GET_ITEM(fv,i); int j=0;
        for(;j<ncells;j++) if(PyUnicode_Compare(name,cellnames[j])==0) break;
        if(j==ncells || !cellv[j]){
            PyErr_Format(PyExc_SystemError,"minipy: no cell for the free variable %R of %R",name,code);
            Py_DECREF(fv); Py_DECREF(clo); return NULL;
        }
        PyTuple_SET_ITEM(clo,i,Py_NewRef(cellv[j]));
    }
    Py_DECREF(fv);
    return clo;
}
PyObject *mpy_func_cpython(PyObject *code, PyObject *globals, PyObject *const *cellv, PyObject *const *cellnames, int ncells,
                           PyObject *defaults, PyObject *kwdefaults, PyObject *annotate, PyObject *annotations){
    PyObject *f=PyFunction_New(code,globals);
    if(!f) return NULL;
    PyObject *clo=closure_for(code,cellv,cellnames,ncells);
    if(!clo){ Py_DECREF(f); return NULL; }
    if(clo!=Py_None && PyFunction_SetClosure(f,clo)){ Py_DECREF(clo); Py_DECREF(f); return NULL; }
    Py_DECREF(clo);
    if(set_func_attrs(f,defaults,kwdefaults,annotate,annotations)){ Py_DECREF(f); return NULL; }
    return f;
}
static PyObject *build_class(PyObject *body, PyObject *name, PyObject *const *bases, Py_ssize_t nbases, PyObject *kwnames, PyObject *const *kwvals, Py_ssize_t nkw){
    PyObject *bc=PyDict_GetItemString(mpy_builtins,"__build_class__");
    if(!bc){ PyErr_SetString(PyExc_NameError,"__build_class__ not found"); return NULL; }
    PyObject *small[16], **argv=small;
    Py_ssize_t total=2+nbases+nkw;
    if(total>16){ argv=(PyObject**)PyMem_Malloc(sizeof(PyObject*)*(size_t)total); if(!argv) return PyErr_NoMemory(); }
    argv[0]=body; argv[1]=name;
    for(Py_ssize_t i=0;i<nbases;i++) argv[2+i]=bases[i];
    for(Py_ssize_t i=0;i<nkw;i++) argv[2+nbases+i]=kwvals[i];
    PyObject *r=PyObject_Vectorcall(bc,argv,(size_t)(2+nbases),nkw?kwnames:NULL);
    if(argv!=small) PyMem_Free(argv);
    return r;
}
PyObject *mpy_class(PyObject *code, PyObject *globals, MpyImpl impl, PyObject *cells, PyObject *name,
                    PyObject *const *bases, Py_ssize_t nbases, PyObject *kwnames, PyObject *const *kwvals, Py_ssize_t nkw){
    PyObject *body=mpy_func(code,globals,impl,cells,NULL,NULL,NULL,NULL,NULL,-1);
    if(!body) return NULL;
    PyObject *r=build_class(body,name,bases,nbases,kwnames,kwvals,nkw);
    Py_DECREF(body);
    return r;
}
PyObject *mpy_class_cpython(PyObject *code, PyObject *globals, PyObject *const *cellv, PyObject *const *cellnames, int ncells, PyObject *name,
                            PyObject *const *bases, Py_ssize_t nbases, PyObject *kwnames, PyObject *const *kwvals, Py_ssize_t nkw){
    PyObject *body=mpy_func_cpython(code,globals,cellv,cellnames,ncells,NULL,NULL,NULL,NULL);
    if(!body) return NULL;
    PyObject *r=build_class(body,name,bases,nbases,kwnames,kwvals,nkw);
    Py_DECREF(body);
    return r;
}
PyObject *mpy_class_ns(void){ return PyEval_GetFrameLocals(); }

/* ---------------------------------------------------------------- names */
static int name_error(PyObject *name){ PyErr_Format(PyExc_NameError,"name '%U' is not defined",name); return -1; }
PyObject *mpy_load_global(PyObject *globals, PyObject *name){
    PyObject *v;
    int r=PyDict_GetItemRef(globals,name,&v);
    if(r) return r<0?NULL:v;
    r=PyDict_GetItemRef(mpy_builtins,name,&v);
    if(r) return r<0?NULL:v;
    name_error(name);
    return NULL;
}
PyObject *mpy_load_name(PyObject *ns, PyObject *globals, PyObject *name){
    PyObject *v;
    int r=PyMapping_GetOptionalItem(ns,name,&v);
    if(r) return r<0?NULL:v;
    return mpy_load_global(globals,name);
}
PyObject *mpy_load_classderef(PyObject *ns, PyObject *cell, PyObject *name){
    PyObject *v;
    int r=PyMapping_GetOptionalItem(ns,name,&v);
    if(r) return r<0?NULL:v;
    return mpy_load_cell(cell,name,1);
}
PyObject *mpy_load_cell(PyObject *cell, PyObject *name, int free){
    PyObject *v=PyCell_Get(cell);
    if(v) return v;
    if(free) PyErr_Format(PyExc_NameError,"cannot access free variable '%U' where it is not associated with a value in enclosing scope",name);
    else mpy_unbound(name);
    return NULL;
}
int mpy_unbound(PyObject *name){
    PyErr_Format(PyExc_UnboundLocalError,"cannot access local variable '%U' where it is not associated with a value",name);
    return -1;
}
int mpy_store_name(PyObject *ns, PyObject *name, PyObject *v){
    return PyDict_CheckExact(ns) ? PyDict_SetItem(ns,name,v) : PyObject_SetItem(ns,name,v);
}
int mpy_delete_name(PyObject *ns, PyObject *name){
    if(PyObject_DelItem(ns,name)==0) return 0;
    if(PyErr_ExceptionMatches(PyExc_KeyError)){ PyErr_Clear(); return name_error(name); }
    return -1;
}
int mpy_delete_global(PyObject *globals, PyObject *name){
    int r=PyDict_Pop(globals,name,NULL);
    if(r<0) return -1;
    if(r==0) return name_error(name);
    return 0;
}

/* ---------------------------------------------------------------- operations */
static int unpack_error_iter(PyObject *v){
    if(PyErr_ExceptionMatches(PyExc_TypeError) && !Py_TYPE(v)->tp_iter && !PySequence_Check(v)){
        PyErr_Clear();
        PyErr_Format(PyExc_TypeError,"cannot unpack non-iterable %.200s object",Py_TYPE(v)->tp_name);
    }
    return -1;
}
int mpy_unpack(PyObject *v, Py_ssize_t n, PyObject **out){
    if((PyTuple_CheckExact(v) && PyTuple_GET_SIZE(v)==n) || (PyList_CheckExact(v) && PyList_GET_SIZE(v)==n)){
        PyObject **items= PyTuple_CheckExact(v) ? ((PyTupleObject*)v)->ob_item : ((PyListObject*)v)->ob_item;
        for(Py_ssize_t i=0;i<n;i++) out[i]=Py_NewRef(items[i]);
        return 0;
    }
    PyObject *it=PyObject_GetIter(v);
    if(!it) return unpack_error_iter(v);
    Py_ssize_t i=0;
    for(;i<n;i++){
        int r=PyIter_NextItem(it,&out[i]);
        if(r<0) goto fail;
        if(r==0){ PyErr_Format(PyExc_ValueError,"not enough values to unpack (expected %zd, got %zd)",n,i); goto fail; }
    }
    PyObject *extra;
    int r=PyIter_NextItem(it,&extra);
    if(r<0) goto fail;
    if(r){
        Py_DECREF(extra);
        if(PyList_CheckExact(v) || PyTuple_CheckExact(v) || PyDict_CheckExact(v)){
            Py_ssize_t len=PyObject_Length(v);
            PyErr_Format(PyExc_ValueError,"too many values to unpack (expected %zd, got %zd)",n,len);
        } else PyErr_Format(PyExc_ValueError,"too many values to unpack (expected %zd)",n);
        goto fail;
    }
    Py_DECREF(it);
    return 0;
fail:
    for(Py_ssize_t k=0;k<i;k++) Py_CLEAR(out[k]);
    Py_DECREF(it);
    return -1;
}
int mpy_unpack_ex(PyObject *v, Py_ssize_t before, Py_ssize_t after, PyObject **out){
    PyObject *it=PyObject_GetIter(v);
    if(!it) return unpack_error_iter(v);
    PyObject *l=PySequence_List(it);
    Py_DECREF(it);
    if(!l) return -1;
    Py_ssize_t len=PyList_GET_SIZE(l);
    if(len<before+after){
        PyErr_Format(PyExc_ValueError,"not enough values to unpack (expected at least %zd, got %zd)",before+after,len);
        Py_DECREF(l); return -1;
    }
    for(Py_ssize_t i=0;i<before;i++) out[i]=Py_NewRef(PyList_GET_ITEM(l,i));
    PyObject *mid=PyList_GetSlice(l,before,len-after);
    if(!mid){ for(Py_ssize_t i=0;i<before;i++) Py_CLEAR(out[i]); Py_DECREF(l); return -1; }
    out[before]=mid;
    for(Py_ssize_t i=0;i<after;i++) out[before+1+i]=Py_NewRef(PyList_GET_ITEM(l,len-after+i));
    Py_DECREF(l);
    return 0;
}
PyObject *mpy_call(PyObject *f, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames){
    return PyObject_Vectorcall(f,args,(size_t)nargs,kwnames);
}
PyObject *mpy_call_method(PyObject *name, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames){
    return PyObject_VectorcallMethod(name,args,(size_t)nargs,kwnames);
}
static const char *func_desc(PyObject *func, char *buf, size_t n){
    PyObject *q=PyObject_GetAttrString(func,"__qualname__");
    if(!q){ PyErr_Clear(); snprintf(buf,n,"%s object",Py_TYPE(func)->tp_name); return buf; }
    const char *s=PyUnicode_Check(q)?PyUnicode_AsUTF8(q):NULL;
    if(s) snprintf(buf,n,"%s()",s); else { PyErr_Clear(); snprintf(buf,n,"function"); }
    Py_DECREF(q); return buf;
}
int mpy_args_extend(PyObject *list, PyObject *it){
    if(PyList_Extend(list,it)==0) return 0;
    if(PyErr_ExceptionMatches(PyExc_TypeError) && !Py_TYPE(it)->tp_iter && !PySequence_Check(it)){
        PyErr_Clear();
        PyErr_Format(PyExc_TypeError,"Value after * must be an iterable, not %.200s",Py_TYPE(it)->tp_name);
    }
    return -1;
}
int mpy_kwargs_merge(PyObject *dict, PyObject *m, PyObject *func){
    char buf[256];
    if(PyDict_CheckExact(m)){
        PyObject *k, *v; Py_ssize_t pos=0;
        while(PyDict_Next(m,&pos,&k,&v)){
            if(!PyUnicode_Check(k)){ PyErr_Format(PyExc_TypeError,"keywords must be strings"); return -1; }
            int has=PyDict_Contains(dict,k);
            if(has<0) return -1;
            if(has){ PyErr_Format(PyExc_TypeError,"%s got multiple values for keyword argument '%U'",func_desc(func,buf,sizeof buf),k); return -1; }
            if(PyDict_SetItem(dict,k,v)) return -1;
        }
        return 0;
    }
    PyObject *keys=PyMapping_Keys(m);
    if(!keys){
        if(PyErr_ExceptionMatches(PyExc_AttributeError) || PyErr_ExceptionMatches(PyExc_TypeError)){
            PyErr_Clear();
            PyErr_Format(PyExc_TypeError,"%s argument after ** must be a mapping, not %.200s",func_desc(func,buf,sizeof buf),Py_TYPE(m)->tp_name);
        }
        return -1;
    }
    PyObject *it=PyObject_GetIter(keys); Py_DECREF(keys);
    if(!it) return -1;
    PyObject *k; int r;
    while((r=PyIter_NextItem(it,&k))==1){
        if(!PyUnicode_Check(k)){ PyErr_Format(PyExc_TypeError,"keywords must be strings"); Py_DECREF(k); Py_DECREF(it); return -1; }
        int has=PyDict_Contains(dict,k);
        if(has){ if(has>0) PyErr_Format(PyExc_TypeError,"%s got multiple values for keyword argument '%U'",func_desc(func,buf,sizeof buf),k); Py_DECREF(k); Py_DECREF(it); return -1; }
        PyObject *v=PyObject_GetItem(m,k);
        if(!v || PyDict_SetItem(dict,k,v)){ Py_XDECREF(v); Py_DECREF(k); Py_DECREF(it); return -1; }
        Py_DECREF(v); Py_DECREF(k);
    }
    Py_DECREF(it);
    return r;
}
PyObject *mpy_call_ex(PyObject *f, PyObject *list, PyObject *dict){
    PyObject *t=PyList_AsTuple(list);
    if(!t) return NULL;
    PyObject *r=PyObject_Call(f,t,dict&&PyDict_GET_SIZE(dict)?dict:NULL);
    Py_DECREF(t);
    return r;
}
int mpy_list_extend(PyObject *list, PyObject *it){
    if(PyList_Extend(list,it)==0) return 0;
    if(PyErr_ExceptionMatches(PyExc_TypeError) && !Py_TYPE(it)->tp_iter && !PySequence_Check(it)){
        PyErr_Clear();
        PyErr_Format(PyExc_TypeError,"Value after * must be an iterable, not %.200s",Py_TYPE(it)->tp_name);
    }
    return -1;
}
int mpy_set_update(PyObject *set, PyObject *v){
    PyObject *it=PyObject_GetIter(v);
    if(!it) return -1;
    PyObject *x; int r;
    while((r=PyIter_NextItem(it,&x))==1){ int e=PySet_Add(set,x); Py_DECREF(x); if(e){ Py_DECREF(it); return -1; } }
    Py_DECREF(it);
    return r;
}
int mpy_dict_update(PyObject *d, PyObject *m){
    if(PyDict_Update(d,m)==0) return 0;
    if(PyErr_ExceptionMatches(PyExc_AttributeError)){
        PyErr_Clear();
        PyErr_Format(PyExc_TypeError,"'%.200s' object is not a mapping",Py_TYPE(m)->tp_name);
    }
    return -1;
}
PyObject *mpy_format(PyObject *v, int conv, PyObject *spec){
    PyObject *c=NULL;
    switch(conv){
        case 's': c=PyObject_Str(v); break;
        case 'r': c=PyObject_Repr(v); break;
        case 'a': c=PyObject_ASCII(v); break;
        default: c=Py_NewRef(v); break;
    }
    if(!c) return NULL;
    if(!spec && PyUnicode_CheckExact(c)) return c;
    PyObject *r=PyObject_Format(c,spec);
    Py_DECREF(c);
    return r;
}
PyObject *mpy_join(PyObject *const *parts, Py_ssize_t n){
    PyObject *t=PyTuple_New(n);
    if(!t) return NULL;
    for(Py_ssize_t i=0;i<n;i++) PyTuple_SET_ITEM(t,i,Py_NewRef(parts[i]));
    PyObject *empty=PyUnicode_FromStringAndSize("",0);
    PyObject *r=empty?PyUnicode_Join(empty,t):NULL;
    Py_XDECREF(empty); Py_DECREF(t);
    return r;
}
PyObject *mpy_compare(PyObject *a, PyObject *b, int op){
    switch(op){
        case 6: case 7:{ int r=PySequence_Contains(b,a); if(r<0) return NULL; return Py_NewRef((r^(op==7))?Py_True:Py_False); }
        case 8: return Py_NewRef(a==b?Py_True:Py_False);
        case 9: return Py_NewRef(a!=b?Py_True:Py_False);
        default: return PyObject_RichCompare(a,b,op);
    }
}
int mpy_truth(PyObject *v){
    if(v==Py_True) return 1;
    if(v==Py_False || v==Py_None) return 0;
    return PyObject_IsTrue(v);
}
PyObject *mpy_slice(PyObject *lo, PyObject *hi, PyObject *step){ return PySlice_New(lo,hi,step); }
PyObject *mpy_iter(PyObject *v){ return PyObject_GetIter(v); }
int mpy_next(PyObject *it, PyObject **item){ return PyIter_NextItem(it,item); }

/* ---------------------------------------------------------------- exceptions */
int mpy_reraise(void){
    PyObject *e=PyErr_GetHandledException();
    if(!e || e==Py_None){ Py_XDECREF(e); PyErr_SetString(PyExc_RuntimeError,"No active exception to reraise"); return -1; }
    PyErr_SetRaisedException(e);
    return -1;
}
static PyObject *exc_instance(PyObject *x){
    if(PyExceptionClass_Check(x)){
        PyObject *v=PyObject_CallNoArgs(x);
        if(!v) return NULL;
        if(!PyExceptionInstance_Check(v)){
            PyErr_Format(PyExc_TypeError,"calling %R should have returned an instance of BaseException, not %s",x,Py_TYPE(v)->tp_name);
            Py_DECREF(v); return NULL;
        }
        return v;
    }
    if(PyExceptionInstance_Check(x)) return Py_NewRef(x);
    return NULL;
}
int mpy_raise(PyObject *exc, PyObject *cause){
    if(!exc) return mpy_reraise();
    PyObject *value=exc_instance(exc);
    if(!value){ if(!PyErr_Occurred()) PyErr_SetString(PyExc_TypeError,"exceptions must derive from BaseException"); return -1; }
    if(cause){
        PyObject *c=NULL;
        if(cause!=Py_None){
            c=exc_instance(cause);
            if(!c){ if(!PyErr_Occurred()) PyErr_SetString(PyExc_TypeError,"exception causes must derive from BaseException"); Py_DECREF(value); return -1; }
        }
        PyException_SetCause(value,c);
    }
    PyErr_SetObject((PyObject*)Py_TYPE(value),value);
    Py_DECREF(value);
    return -1;
}
static int check_exc_type(PyObject *t){
    if(PyTuple_Check(t)){
        for(Py_ssize_t i=0;i<PyTuple_GET_SIZE(t);i++) if(!PyExceptionClass_Check(PyTuple_GET_ITEM(t,i))) goto bad;
        return 0;
    }
    if(PyExceptionClass_Check(t)) return 0;
bad:
    PyErr_SetString(PyExc_TypeError,"catching classes that do not inherit from BaseException is not allowed");
    return -1;
}
int mpy_exc_matches(PyObject *exc, PyObject *type){
    if(check_exc_type(type)) return -1;
    return PyErr_GivenExceptionMatches(exc,type);
}
int mpy_assert_fail(PyObject *msg){
    PyObject *v= msg ? PyObject_CallOneArg(PyExc_AssertionError,msg) : PyObject_CallNoArgs(PyExc_AssertionError);
    if(!v) return -1;
    PyErr_SetObject(PyExc_AssertionError,v);
    Py_DECREF(v);
    return -1;
}
static PyObject *lookup_special(PyObject *o, const char *name){
    PyObject *n=PyUnicode_FromString(name);
    if(!n) return NULL;
    PyObject *d=_PyType_Lookup(Py_TYPE(o),n);
    Py_DECREF(n);
    if(!d) return NULL;
    descrgetfunc g=Py_TYPE(d)->tp_descr_get;
    return g ? g(d,o,(PyObject*)Py_TYPE(o)) : Py_NewRef(d);
}
PyObject *mpy_with_enter(PyObject *mgr, PyObject **exit_out, int is_async){
    const char *en=is_async?"__aenter__":"__enter__", *ex=is_async?"__aexit__":"__exit__";
    PyObject *enter=lookup_special(mgr,en);
    if(!enter){
        if(!PyErr_Occurred()) PyErr_Format(PyExc_TypeError,"'%.200s' object does not support the %scontext manager protocol",Py_TYPE(mgr)->tp_name,is_async?"asynchronous ":"");
        return NULL;
    }
    PyObject *exit=lookup_special(mgr,ex);
    if(!exit){
        if(!PyErr_Occurred()) PyErr_Format(PyExc_TypeError,"'%.200s' object does not support the %scontext manager protocol (missed %s method)",Py_TYPE(mgr)->tp_name,is_async?"asynchronous ":"",ex);
        Py_DECREF(enter); return NULL;
    }
    PyObject *r=PyObject_CallNoArgs(enter);
    Py_DECREF(enter);
    if(!r){ Py_DECREF(exit); return NULL; }
    *exit_out=exit;
    return r;
}
int mpy_with_exit(PyObject *exit, PyObject *exc){
    PyObject *r;
    if(!exc) r=PyObject_CallFunctionObjArgs(exit,Py_None,Py_None,Py_None,NULL);
    else {
        PyObject *tb=PyException_GetTraceback(exc);
        r=PyObject_CallFunctionObjArgs(exit,(PyObject*)Py_TYPE(exc),exc,tb?tb:Py_None,NULL);
        Py_XDECREF(tb);
    }
    if(!r) return -1;
    if(!exc){ Py_DECREF(r); return 0; }
    int t=PyObject_IsTrue(r);
    Py_DECREF(r);
    return t;
}

/* ---------------------------------------------------------------- imports */
PyObject *mpy_import(PyObject *globals, PyObject *name, PyObject *fromlist, int level){
    return PyImport_ImportModuleLevelObject(name,globals,NULL,fromlist?fromlist:Py_None,level);
}
PyObject *mpy_import_from(PyObject *module, PyObject *name){
    PyObject *x;
    int r=PyObject_GetOptionalAttr(module,name,&x);
    if(r) return r<0?NULL:x;
    PyObject *pkg=NULL, *full=NULL, *path=NULL;
    if(PyObject_GetOptionalAttrString(module,"__name__",&pkg)<0) return NULL;
    if(pkg && PyUnicode_Check(pkg)){
        full=PyUnicode_FromFormat("%U.%U",pkg,name);
        if(!full){ Py_DECREF(pkg); return NULL; }
        x=PyImport_GetModule(full);
        Py_DECREF(full);
        if(x){ Py_DECREF(pkg); return x; }
        if(PyErr_Occurred()){ Py_DECREF(pkg); return NULL; }
    }
    if(PyObject_GetOptionalAttrString(module,"__file__",&path)<0) PyErr_Clear();
    PyObject *msg;
    if(pkg && PyUnicode_Check(pkg)){
        int init=0; PyObject *spec=NULL;
        if(PyObject_GetOptionalAttrString(module,"__spec__",&spec)>0 && spec!=Py_None){
            PyObject *v=NULL; if(PyObject_GetOptionalAttrString(spec,"_initializing",&v)>0){ init=PyObject_IsTrue(v)>0; Py_DECREF(v); }
        }
        PyErr_Clear(); Py_XDECREF(spec);
        if(init) msg=PyUnicode_FromFormat("cannot import name %R from partially initialized module %R (most likely due to a circular import) (%S)",name,pkg,path?path:Py_None);
        else if(path) msg=PyUnicode_FromFormat("cannot import name %R from %R (%S)",name,pkg,path);
        else msg=PyUnicode_FromFormat("cannot import name %R from %R (unknown location)",name,pkg);
    } else msg=PyUnicode_FromFormat("cannot import name %R from <unknown module name>",name);
    if(msg){ PyErr_SetImportError(msg,pkg&&PyUnicode_Check(pkg)?pkg:NULL,path); Py_DECREF(msg); }
    Py_XDECREF(pkg); Py_XDECREF(path);
    return NULL;
}
int mpy_import_star(PyObject *module, PyObject *ns){
    PyObject *all=NULL, *names=NULL; int skip_private=0;
    if(PyObject_GetOptionalAttrString(module,"__all__",&all)<0) return -1;
    if(all) names=all;
    else {
        PyObject *d=PyObject_GetAttrString(module,"__dict__");
        if(!d){ PyErr_SetString(PyExc_ImportError,"from-import-* object has no __dict__ and no __all__"); return -1; }
        names=PyMapping_Keys(d); Py_DECREF(d);
        if(!names) return -1;
        skip_private=1;
    }
    PyObject *it=PyObject_GetIter(names); Py_DECREF(names);
    if(!it) return -1;
    PyObject *k; int r;
    while((r=PyIter_NextItem(it,&k))==1){
        if(!PyUnicode_Check(k)){
            PyObject *mn=PyObject_GetAttrString(module,"__name__");
            PyErr_Format(PyExc_TypeError,"%s in %U.%s must be str, not %.100s",skip_private?"Key":"Item",mn?mn:Py_None,skip_private?"__dict__":"__all__",Py_TYPE(k)->tp_name);
            Py_XDECREF(mn); Py_DECREF(k); Py_DECREF(it); return -1;
        }
        if(skip_private && PyUnicode_READ_CHAR(k,0)=='_'){ Py_DECREF(k); continue; }
        PyObject *v=PyObject_GetAttr(module,k);
        int e= !v || mpy_store_name(ns,k,v);
        Py_XDECREF(v); Py_DECREF(k);
        if(e){ Py_DECREF(it); return -1; }
    }
    Py_DECREF(it);
    return r;
}

void mpy_traceback(const char *func, const char *file, int line){
    if(!PyErr_Occurred()) return;
    /* the callee's trampoline frame said its def line: the C's entries say where */
    PyObject *exc=PyErr_GetRaisedException();
    PyObject *tb=PyException_GetTraceback(exc);
    if(tb){
        PyTracebackObject *t=(PyTracebackObject*)tb;
        PyCodeObject *c=PyFrame_GetCode(t->tb_frame);
        if(mpy_stubs && PySet_Contains(mpy_stubs,(PyObject*)c)>0) PyException_SetTraceback(exc,t->tb_next?(PyObject*)t->tb_next:Py_None);
        Py_DECREF(c); Py_DECREF(tb);
    }
    PyErr_SetRaisedException(exc);
    _PyTraceback_Add(func,file,line);
}
/* a statement this compiler leaves to CPython: its source, at its line, run in the module / class body */
int mpy_exec_source(PyObject **cache, const char *src, const char *file, int future_flags, PyObject *globals, PyObject *locals){
    if(!*cache){
        PyCompilerFlags cf={future_flags,PY_MINOR_VERSION};
        *cache=Py_CompileStringExFlags(src,file,Py_file_input,&cf,-1);
        if(!*cache) return -1;
    }
    PyObject *r=PyEval_EvalCode(*cache,globals,locals);
    if(!r) return -1;
    Py_DECREF(r);
    return 0;
}
int mpy_prepare_globals(PyObject *g){
    if(watch(g)) return -1;
    if(mpy_builtins && watch(mpy_builtins)) return -1;
    int r=PyDict_ContainsString(g,"__builtins__");
    if(r) return r<0 ? -1 : 0;
    PyObject *b=PyImport_ImportModule("builtins");
    if(!b) return -1;
    r=PyDict_SetItemString(g,"__builtins__",b);
    Py_DECREF(b);
    return r;
}

/* ---------------------------------------------------------------- the module loader and main */
static const MpyModule *mods; static int nmods;
static const MpyModule *find_module(PyObject *name){
    const char *s=PyUnicode_AsUTF8(name);
    if(!s){ PyErr_Clear(); return NULL; }
    for(int i=0;i<nmods;i++) if(!strcmp(mods[i].name,s)) return &mods[i];
    return NULL;
}
static PyObject *ld_lookup(PyObject *self, PyObject *name){
    (void)self;
    const MpyModule *m=find_module(name);
    if(!m) Py_RETURN_NONE;
    return Py_BuildValue("(sO)",m->file,m->is_package?PyUnicode_FromString(m->dir):Py_NewRef(Py_None));
}
static PyObject *ld_exec(PyObject *self, PyObject *module){
    (void)self;
    PyObject *name=PyObject_GetAttrString(module,"__name__");
    if(!name) return NULL;
    const MpyModule *m=find_module(name);
    Py_DECREF(name);
    if(!m){ PyErr_SetString(PyExc_ImportError,"minipy: not a compiled module"); return NULL; }
    if(m->exec(module)) return NULL;
    Py_RETURN_NONE;
}
static PyMethodDef ld_methods[]={{"lookup",ld_lookup,METH_O,NULL},{"exec_module",ld_exec,METH_O,NULL},{NULL,NULL,0,NULL}};
static struct PyModuleDef ld_def={PyModuleDef_HEAD_INIT,"_minipy",NULL,-1,ld_methods,NULL,NULL,NULL,NULL};
static PyObject *ld_init(void){ return PyModule_Create(&ld_def); }

static const char bootstrap[]=
    "import sys, _minipy\n"
    "from importlib.machinery import ModuleSpec\n"
    "class MinipyLoader:\n"
    "    @staticmethod\n"
    "    def create_module(spec): return None\n"
    "    @staticmethod\n"
    "    def exec_module(module): _minipy.exec_module(module)\n"
    "    @staticmethod\n"
    "    def get_source(fullname):\n"
    "        info = _minipy.lookup(fullname)\n"
    "        if info is None: return None\n"
    "        with open(info[0], 'rb') as f: data = f.read()\n"
    "        import importlib.util\n"
    "        return importlib.util.decode_source(data)\n"
    "    @staticmethod\n"
    "    def is_package(fullname):\n"
    "        info = _minipy.lookup(fullname)\n"
    "        return info is not None and info[1] is not None\n"
    "class MinipyFinder:\n"
    "    @classmethod\n"
    "    def find_spec(cls, fullname, path=None, target=None):\n"
    "        info = _minipy.lookup(fullname)\n"
    "        if info is None: return None\n"
    "        spec = ModuleSpec(fullname, MinipyLoader, origin=info[0], is_package=info[1] is not None)\n"
    "        spec.has_location = True\n"
    "        if info[1] is not None: spec.submodule_search_locations = [info[1]]\n"
    "        return spec\n"
    "    @classmethod\n"
    "    def invalidate_caches(cls): pass\n"
    "sys.meta_path.insert(0, MinipyFinder)\n";

int mpy_main(int argc, char **argv, const MpyModule *mlist, int n, const char *python, const char *script_dir){
    mods=mlist; nmods=n;
    if(PyImport_AppendInittab("_minipy",ld_init)<0) return 1;
    PyConfig config; PyConfig_InitPythonConfig(&config);
    config.parse_argv=0;
    PyStatus st=PyConfig_SetBytesArgv(&config,argc,argv);
    if(!PyStatus_Exception(st) && python) st=PyConfig_SetBytesString(&config,&config.program_name,python);   /* sys.prefix, the venv, site-packages: as for that python */
    if(!PyStatus_Exception(st)) st=Py_InitializeFromConfig(&config);
    PyConfig_Clear(&config);
    if(PyStatus_Exception(st)) Py_ExitStatusException(st);
    int rc=0;
    PyObject *bi=PyImport_ImportModule("builtins");
    if(!bi){ PyErr_Print(); return 1; }
    mpy_builtins=Py_NewRef(PyModule_GetDict(bi)); Py_DECREF(bi);
    PyObject *sysmod=PyImport_ImportModule("sys"), *path=sysmod?PyObject_GetAttrString(sysmod,"path"):NULL;
    PyObject *dir=PyUnicode_FromString(script_dir);
    if(!path || !dir || PyList_Insert(path,0,dir)){ PyErr_Print(); return 1; }
    Py_DECREF(dir); Py_DECREF(path); Py_DECREF(sysmod);
    PyObject *g=PyDict_New();
    PyObject *r=g?PyRun_String(bootstrap,Py_file_input,g,g):NULL;
    Py_XDECREF(g);
    if(!r){ PyErr_Print(); return 1; }
    Py_DECREF(r);
    /* __main__ */
    PyObject *main=PyImport_AddModuleRef("__main__");
    if(!main){ PyErr_Print(); return 1; }
    PyObject *md=PyModule_GetDict(main);
    PyObject *file=PyUnicode_FromString(mlist[0].file);
    if(!file || PyDict_SetItemString(md,"__file__",file) || PyDict_SetItemString(md,"__cached__",Py_None)){ PyErr_Print(); return 1; }
    Py_DECREF(file);
    if(mlist[0].exec(main)){
        /* as python does: what the program printed comes before the traceback */
        PyObject *exc=PyErr_GetRaisedException();
        if(exc) strip_exc(exc);
        PyObject *out=PySys_GetObject("stdout");
        if(out && out!=Py_None){ PyObject *r=PyObject_CallMethod(out,"flush",NULL); if(r) Py_DECREF(r); else PyErr_Clear(); }
        PyErr_SetRaisedException(exc);
        PyErr_Print();                               /* SystemExit: exits with its code */
        rc=1;
    }
    Py_DECREF(main);
    if(Py_FinalizeEx()<0) rc=120;
    return rc;
}
