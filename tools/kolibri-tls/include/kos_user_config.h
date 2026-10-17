/* plainpy's changes to the port's config.h (MBEDTLS_USER_CONFIG_FILE: the
   public structures stay as the port's mbedtls.obj has them) */
#undef MBEDTLS_PADLOCK_C          /* (VIA's: its inline assembly's labels clash in one module) */
#undef MBEDTLS_SELF_TEST          /* (the self tests: size, never run) */
