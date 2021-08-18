#include "common.h"

#define LOG_PREFIX_ "upd.sound.dev: "

#define DEFAULT_CH_    1
#define DEFAULT_SRATE_ 48000

#define RING_DURATION_ 10  /* = 10 sec */


static
bool
dev_init_(
  upd_file_t* f);

static
bool
dev_parse_param_(
  upd_file_t* f);

static
bool
dev_prepare_(
  upd_file_t* f);

static
void
dev_deinit_(
  upd_file_t* f);

static
bool
dev_handle_(
  upd_req_t* req);

static
void
dev_playback_cb_(
  ma_device*  ma,
  void*       out,
  const void* in,
  ma_uint32   frame);

const upd_driver_t snd_dev = {
  .name   = (uint8_t*) "upd.sound.dev",
  .cats   = (upd_req_cat_t[]) {
    UPD_REQ_PROG,
    0,
  },
  .init   = dev_init_,
  .deinit = dev_deinit_,
  .handle = dev_handle_,
};


static ma_context    ma_        = {0};
static atomic_size_t ma_refcnt_ = ATOMIC_VAR_INIT(0);

static bool dev_init_(upd_file_t* f) {
  upd_iso_t* iso = f->iso;

  if (HEDLEY_UNLIKELY(atomic_fetch_add(&ma_refcnt_, 1) == 0)) {
    if (HEDLEY_UNLIKELY(ma_context_init(NULL, 0, NULL, &ma_) != MA_SUCCESS)) {
      return false;
    }
  }

  snd_dev_t* ctx = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&ctx, sizeof(*ctx)))) {
    goto ABORT;
  }
  *ctx = (snd_dev_t) {
    .type = ma_device_type_playback,
  };
  f->ctx = ctx;

  if (HEDLEY_UNLIKELY(!dev_parse_param_(f))) {
    upd_free(&ctx);
    goto ABORT;
  }

  ctx->samples = ctx->srate * ctx->ch * RING_DURATION_;

  const size_t ringsz = sizeof(*ctx->ring) * ctx->samples;
  if (HEDLEY_UNLIKELY(!upd_malloc(&ctx->ring, ringsz))) {
    upd_free(&ctx);
    goto ABORT;
  }

  if (HEDLEY_UNLIKELY(ma_mutex_init(&ctx->mtx) != MA_SUCCESS)) {
    upd_free(&ctx->ring);
    upd_free(&ctx);
    goto ABORT;
  }

  if (HEDLEY_UNLIKELY(!dev_prepare_(f))) {
    ma_mutex_uninit(&ctx->mtx);
    upd_free(&ctx->ring);
    upd_free(&ctx);
    upd_iso_msgf(iso, LOG_PREFIX_"failed to prepare sound device\n");
    goto ABORT;
  }
  return true;

ABORT:
  if (HEDLEY_UNLIKELY(atomic_fetch_sub(&ma_refcnt_, 1) == 1)) {
    ma_context_uninit(&ma_);
  }
  return false;
}

static bool dev_parse_param_(upd_file_t* f) {
  upd_iso_t* iso = f->iso;
  snd_dev_t* ctx = f->ctx;

  bool ok = false;

  yaml_document_t doc;
  if (HEDLEY_UNLIKELY(!upd_yaml_parse(&doc, f->param, f->paramlen))) {
    upd_iso_msgf(iso, LOG_PREFIX_"param parse failure\n");
    goto EXIT;
  }

  const yaml_node_t* name = NULL;

  uintmax_t ch    = DEFAULT_CH_;
  uintmax_t srate = DEFAULT_SRATE_;

  const char* invalid =
    upd_yaml_find_fields_from_root(&doc, (upd_yaml_field_t[]) {
        { .name = "name",    .str = &name,         },
        { .name = "verbose", .b   = &ctx->verbose, },
        { .name = "ch",      .ui  = &ch,           },
        { .name = "srate",   .ui  = &srate,        },
        { NULL, },
      });
  if (HEDLEY_UNLIKELY(invalid)) {
    upd_iso_msgf(iso, LOG_PREFIX_"invalid param (%s)\n", invalid);
    goto EXIT;
  }

  ctx->ch    = ch;
  ctx->srate = srate;

  if (name) {
    if (HEDLEY_UNLIKELY(name->data.scalar.length >= sizeof(ctx->name))) {
      upd_iso_msgf(iso, LOG_PREFIX_"too long name pattern\n");
      goto EXIT;
    }
    utf8ncpy(ctx->name, name->data.scalar.value, name->data.scalar.length);
  }

  ok = true;
EXIT:
  yaml_document_delete(&doc);
  return ok;
}

static ma_bool32 dev_prepare_enum_dev_cb_(
    ma_context*           ma,
    ma_device_type        type,
    const ma_device_info* info,
    void*                 udata) {
  upd_file_t* f   = udata;
  upd_iso_t*  iso = f->iso;
  snd_dev_t*  ctx = f->ctx;
  (void) ma;

  upd_iso_msgf(iso, LOG_PREFIX_"sound device found: %s\n", info->name);
  if (HEDLEY_UNLIKELY(type != ctx->type)) {
    return MA_TRUE;
  }

  if (HEDLEY_UNLIKELY(ctx->name[0] == 0)) {
    if (HEDLEY_UNLIKELY(info->isDefault)) {
      goto MATCH;
    }
  } else {
    int matchlen = 0;
    re_match((char*) ctx->name, info->name, &matchlen);
    if (HEDLEY_UNLIKELY(info->name[matchlen] == 0)) {
      goto MATCH;
    }
  }
  return MA_TRUE;

MATCH:
  upd_iso_msgf(iso, LOG_PREFIX_"sound device matched: '%s'\n", info->name);

  ctx->id    = info->id;
  ctx->found = true;
  return MA_FALSE;
}
static bool dev_prepare_(upd_file_t* f) {
  snd_dev_t* ctx = f->ctx;

  const ma_result ret =
    ma_context_enumerate_devices(&ma_, dev_prepare_enum_dev_cb_, f);
  if (HEDLEY_UNLIKELY(ret != MA_SUCCESS || !ctx->found)) {
    return false;
  }

  ma_device_config config = ma_device_config_init(ctx->type);
  config.playback.pDeviceID = &ctx->id;
  config.playback.format    = ma_format_f32;
  config.playback.channels  = ctx->ch;
  config.sampleRate         = ctx->srate;
  config.dataCallback       = dev_playback_cb_;
  config.pUserData          = f;

  if (HEDLEY_UNLIKELY(ma_device_init(&ma_, &config, &ctx->ma) != MA_SUCCESS)) {
    return false;
  }
  ctx->ma.pUserData = f;
  if (HEDLEY_UNLIKELY(ma_device_start(&ctx->ma) != MA_SUCCESS)) {
    return false;
  }
  return true;
}

static void dev_deinit_(upd_file_t* f) {
  snd_dev_t* ctx = f->ctx;

  ma_device_uninit(&ctx->ma);
  ma_mutex_uninit(&ctx->mtx);

  upd_free(&ctx->ring);
  upd_free(&ctx);

  if (HEDLEY_UNLIKELY(atomic_fetch_sub(&ma_refcnt_, 1) == 1)) {
    ma_context_uninit(&ma_);
  }
}

static bool dev_handle_(upd_req_t* req) {
  upd_file_t* f = req->file;

  switch (req->type) {
  case UPD_REQ_PROG_EXEC: {
    upd_file_t* stf = upd_file_new(&(upd_file_t) {
        .iso    = f->iso,
        .driver = &snd_stream,
      });
    if (HEDLEY_UNLIKELY(stf == NULL)) {
      req->result = UPD_REQ_NOMEM;
      return false;
    }
    snd_stream_t* stctx = stf->ctx;
    stctx->dev = f;
    upd_file_ref(f);

    req->prog.exec = stf;
    req->result    = UPD_REQ_OK;
    req->cb(req);
    upd_file_unref(stf);
  } return true;

  default:
    req->result = UPD_REQ_INVALID;
    return false;
  }
}

static void dev_playback_cb_(
    ma_device* ma, void* out, const void* in, ma_uint32 frame) {
  upd_file_t* f = ma->pUserData;
  (void) in;

  if (HEDLEY_UNLIKELY(f == NULL)) {
    return;
  }
  snd_dev_t* ctx = f->ctx;

  const size_t need_samples = frame * ctx->ch;
  atomic_fetch_add(&ctx->now, need_samples);

  uint8_t* src = (void*) ctx->ring;
  uint8_t* dst = out;

  ma_mutex_lock(&ctx->mtx);
  const size_t offset = ctx->tail;

  if (ctx->head > ctx->tail) {
    size_t actual_samples = ctx->head - ctx->tail;
    if (actual_samples > need_samples) {
      actual_samples = need_samples;
    }
    memcpy(dst,                  src+offset*4, actual_samples*4);
    memset(dst+actual_samples*4, 0,            (need_samples-actual_samples)*4);

    ctx->tail += actual_samples;

  } else if (ctx->head < ctx->tail) {
    size_t latter_samples = (ctx->samples - ctx->tail) * ctx->ch;
    size_t former_samples = (ctx->head) * ctx->ch;

    size_t actual_samples = latter_samples + former_samples;
    if (actual_samples > need_samples) {
      actual_samples = need_samples;
    }
    if (actual_samples <= latter_samples) {
      memcpy(dst,                  src+offset*4, actual_samples*4);
      memset(dst+actual_samples*4, 0,            (need_samples-actual_samples)*4);
      ctx->tail += actual_samples;
    } else {
      former_samples -= actual_samples - latter_samples;
      memcpy(dst,                  src+offset*4, latter_samples*4);
      memcpy(dst+latter_samples*4, src,          former_samples*4);
      memset(dst+actual_samples*4, 0,            (need_samples-actual_samples)*4);
      ctx->tail = former_samples;
    }

  } else {
    memset(dst, 0, need_samples*4);
  }
  ma_mutex_unlock(&ctx->mtx);
}
