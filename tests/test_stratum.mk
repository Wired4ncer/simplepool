build/test_stratum: tests/test_stratum.c src/stratum.c src/coinbase.c \
                    src/share.c src/sha256.c src/thunder.c \
                    src/log.c src/cjson/cJSON.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -o build/test_stratum $(filter %.c,$^) -lpthread
