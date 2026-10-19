/* Force-included into every file of the component: the driver carries a small pbuf emulation of its own
 * (net_al.c); its functions get other names so that they do not meet those of lwIP at the link. */
#ifndef AIC_RENAME_H
#define AIC_RENAME_H

#define pbuf_alloc  aic_pbuf_alloc
#define pbuf_free   aic_pbuf_free
#define pbuf_header aic_pbuf_header
#define pbuf_ref    aic_pbuf_ref

#endif
