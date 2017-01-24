/* -*- mode: C; c-basic-offset: 4; indent-tabs-mode: nil; -*- */
/*
 * Copyright (c) 2008  litl, LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <config.h>

#include "ns.h"
#include "repo.h"
#include "param.h"
#include "gjs/jsapi-wrapper.h"
#include "gjs/mem.h"

#include <util/log.h>
#include <girepository.h>

#include <string.h>

typedef struct {
    char *gi_namespace;
} Ns;

extern struct JSClass gjs_ns_class;

GJS_DEFINE_PRIV_FROM_JS(Ns, gjs_ns_class)

/*
 * The *objp out parameter, on success, should be null to indicate that id
 * was not resolved; and non-null, referring to obj or one of its prototypes,
 * if id was resolved.
 */
static bool
ns_new_resolve(JSContext *context,
               JS::HandleObject obj,
               JS::HandleId id,
               JS::MutableHandleObject objp)
{
    Ns *priv;
    char *name;
    GIRepository *repo;
    GIBaseInfo *info;
    bool ret = false;
    bool defined;

    if (!gjs_get_string_id(context, id, &name))
        return true; /* not resolved, but no error */

    /* let Object.prototype resolve these */
    if (strcmp(name, "valueOf") == 0 ||
        strcmp(name, "toString") == 0) {
        ret = true;
        goto out;
    }

    priv = priv_from_js(context, obj);
    gjs_debug_jsprop(GJS_DEBUG_GNAMESPACE,
                     "Resolve prop '%s' hook obj %p priv %p",
                     name, obj.get(), priv);

    if (priv == NULL) {
        ret = true; /* we are the prototype, or have the wrong class */
        goto out;
    }

    JS_BeginRequest(context);

    repo = g_irepository_get_default();

    info = g_irepository_find_by_name(repo, priv->gi_namespace, name);
    if (info == NULL) {
        /* No property defined, but no error either, so return true */
        JS_EndRequest(context);
        ret = true;
        goto out;
    }

    gjs_debug(GJS_DEBUG_GNAMESPACE,
              "Found info type %s for '%s' in namespace '%s'",
              gjs_info_type_name(g_base_info_get_type(info)),
              g_base_info_get_name(info),
              g_base_info_get_namespace(info));

    if (gjs_define_info(context, obj, info, &defined)) {
        g_base_info_unref(info);
        if (defined)
            objp.set(obj); /* we defined the property in this object */
        ret = true;
    } else {
        gjs_debug(GJS_DEBUG_GNAMESPACE,
                  "Failed to define info '%s'",
                  g_base_info_get_name(info));

        g_base_info_unref(info);
    }
    JS_EndRequest(context);

 out:
    g_free(name);
    return ret;
}

static bool
get_name (JSContext *context,
          unsigned   argc,
          JS::Value *vp)
{
    GJS_GET_PRIV(context, argc, vp, args, obj, Ns, priv);

    if (priv == NULL)
        return false;

    return gjs_string_from_utf8(context, priv->gi_namespace, -1, args.rval());
}

GJS_NATIVE_CONSTRUCTOR_DEFINE_ABSTRACT(ns)

static void
ns_finalize(JSFreeOp *fop,
            JSObject *obj)
{
    Ns *priv;

    priv = (Ns *)JS_GetPrivate(obj);
    gjs_debug_lifecycle(GJS_DEBUG_GNAMESPACE,
                        "finalize, obj %p priv %p", obj, priv);
    if (priv == NULL)
        return; /* we are the prototype, not a real instance */

    if (priv->gi_namespace)
        g_free(priv->gi_namespace);

    GJS_DEC_COUNTER(ns);
    g_slice_free(Ns, priv);
}

/* The bizarre thing about this vtable is that it applies to both
 * instances of the object, and to the prototype that instances of the
 * class have.
 */
struct JSClass gjs_ns_class = {
    "GIRepositoryNamespace",
    JSCLASS_HAS_PRIVATE |
    JSCLASS_NEW_RESOLVE |
    JSCLASS_IMPLEMENTS_BARRIERS,
    JS_PropertyStub,
    JS_DeletePropertyStub,
    JS_PropertyStub,
    JS_StrictPropertyStub,
    JS_EnumerateStub,
    (JSResolveOp) ns_new_resolve, /* needs cast since it's the new resolve signature */
    JS_ConvertStub,
    ns_finalize
};

JSPropertySpec gjs_ns_proto_props[] = {
    JS_PSG("__name__", get_name, GJS_MODULE_PROP_FLAGS),
    JS_PS_END
};

JSFunctionSpec gjs_ns_proto_funcs[] = {
    JS_FS_END
};

static bool
ns_new(JSContext              *context,
       const char             *ns_name,
       JS::MutableHandleObject ns)
{
    Ns *priv;
    bool found;

    /* put constructor in the global namespace */
    JS::RootedObject global(context, gjs_get_import_global(context));

    if (!JS_HasProperty(context, global, gjs_ns_class.name, &found))
        return false;
    if (!found) {
        JSObject *prototype;
        prototype = JS_InitClass(context, global,
                                 /* parent prototype JSObject* for
                                  * prototype; NULL for
                                  * Object.prototype
                                  */
                                 JS::NullPtr(),
                                 &gjs_ns_class,
                                 /* constructor for instances (NULL for
                                  * none - just name the prototype like
                                  * Math - rarely correct)
                                  */
                                 gjs_ns_constructor,
                                 /* number of constructor args */
                                 0,
                                 /* props of prototype */
                                 &gjs_ns_proto_props[0],
                                 /* funcs of prototype */
                                 &gjs_ns_proto_funcs[0],
                                 /* props of constructor, MyConstructor.myprop */
                                 NULL,
                                 /* funcs of constructor, MyConstructor.myfunc() */
                                 NULL);
        if (prototype == NULL)
            g_error("Can't init class %s", gjs_ns_class.name);

        gjs_debug(GJS_DEBUG_GNAMESPACE, "Initialized class %s prototype %p",
                  gjs_ns_class.name, prototype);
    }

    ns.set(JS_NewObject(context, &gjs_ns_class, JS::NullPtr(), global));
    if (ns == NULL)
        g_error("No memory to create ns object");

    priv = g_slice_new0(Ns);

    GJS_INC_COUNTER(ns);

    g_assert(priv_from_js(context, ns) == NULL);
    JS_SetPrivate(ns, priv);

    gjs_debug_lifecycle(GJS_DEBUG_GNAMESPACE, "ns constructor, obj %p priv %p",
                        ns.get(), priv);

    priv = priv_from_js(context, ns);
    priv->gi_namespace = g_strdup(ns_name);
    return true;
}

bool
gjs_import_gi_module(JSContext              *cx,
                     const char             *module_name,
                     const char             *module_version,
                     JS::MutableHandleObject module)
{
    GIRepository *repo = g_irepository_get_default();
    GError *error = NULL;

    if (!g_irepository_require(repo, module_name, module_version, (GIRepositoryLoadFlags) 0, &error)) {
        gjs_throw_g_error(cx, error);
        g_clear_error(&error);
        return false;
    }

    return ns_new(cx, module_name, module);
}
