
/**
 * @file    STM32/rtc_lld.h
 * @brief   STM32 RTC subsystem low level driver header.
 *
 * @addtogroup RTC
 * @{
 */

#ifndef _RTC_LLD_H_
#define _RTC_LLD_H_

#if HAL_USE_RTC || defined(__DOXYGEN__)

/*===========================================================================*/
/* Driver constants.                                                         */
/*===========================================================================*/

/*===========================================================================*/
/* Driver pre-compile time settings.                                         */
/*===========================================================================*/
/**
 * @brief   Switch to TRUE if you need callbacks from RTC. Switch to FALSE
 *          if you need only time keeping.
 * @note    Default is true.
 */
#if !defined(RTC_SUPPORTS_CALLBACKS) || defined(__DOXYGEN__)
#define RTC_SUPPORTS_CALLBACKS              TRUE
#endif

/*===========================================================================*/
/* Derived constants and error checks.                                       */
/*===========================================================================*/

#if HAL_USE_RTC && !STM32_HAS_RTC
#error "RTC not present in the selected device"
#endif

/*===========================================================================*/
/* Driver data structures and types.                                         */
/*===========================================================================*/
/**
 * @brief Structure representing an RTC driver config.
 */
typedef struct {
  /**
   * @brief Overflow callback. Set it to NULL if not used.
   */
  rtccb_t           overflow_cb;

  /**
   * @brief Every second callback. Set it to NULL if not used.
   */
  rtccb_t           second_cb;

  /**
   * @brief Alarm callback. Set it to NULL if not used.
   */
  rtccb_t           alarm_cb;
}RTCConfig;


/**
 * @brief Structure representing an RTC driver.
 */
struct RTCDriver{
  /**
   * @brief Pointer to RCT config.
   */
  const RTCConfig       *config;
};

/*===========================================================================*/
/* Driver macros.                                                            */
/*===========================================================================*/

/*===========================================================================*/
/* External declarations.                                                    */
/*===========================================================================*/

#ifdef __cplusplus
extern "C" {
#endif
  void rtc_lld_init(void);
#if RTC_SUPPORTS_CALLBACKS
  void rtc_lld_start(RTCDriver *rtcp, RTCConfig *rtccfgp);
  void rtc_lld_stop(void);
#endif /* RTC_SUPPORTS_CALLBACKS */
  void rtc_lld_set_time(uint32_t tv_sec);
  uint32_t rtc_lld_get_sec(void);
  uint16_t rtc_lld_get_msec(void);
#ifdef __cplusplus
}
#endif


#endif /* HAL_USE_RTC */
#endif /* _RTC_LLD_H_ */

/** @} */
