#include "net/http.h"
int main(int argc, char **argv) {
    const char *url = argc > 1 ? argv[1] : "https://example.com/";
    http_resp_t r;
    for (int i = 0; i < 2; i++) {
        int64_t t = mono_ms();
        int rc = http_get(url, NULL, 20000, &r);
        printf("rc=%d status=%d len=%zu err=%s t=%lldms enc=%s\n", rc, r.status, r.body_len, r.error ? r.error : "-", (long long)(mono_ms()-t), http_headers_get(&r.headers,"Content-Encoding"));
        if (r.body) printf("%.80s\n", r.body);
        http_resp_free(&r);
    }
    return 0;
}
