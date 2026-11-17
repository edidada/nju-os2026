#define main gpt_program_main
#include "../gpt/gpt.c"
#undef main

static void check_case(int B, int T, int C, int OC, int with_bias) {
    size_t rows = (size_t)B * T, n = rows * OC;
    float *inp = malloc(rows * C * sizeof(float));
    float *weight = malloc((size_t)OC * C * sizeof(float));
    float *bias = malloc((size_t)OC * sizeof(float));
    float *out = malloc((n + 2) * sizeof(float));
    float *expected = malloc(n * sizeof(float));
    assert(inp && weight && bias && out && expected);
    for (size_t i = 0; i < rows * C; i++) inp[i] = ((int)(i % 131) - 65) / 64.0f;
    for (size_t i = 0; i < (size_t)OC * C; i++) weight[i] = ((int)(i % 113) - 56) / 32.0f;
    for (int o = 0; o < OC; o++) bias[o] = (o % 17 - 8) / 16.0f;
    out[0] = out[n + 1] = 12345.0f;
    for (size_t row = 0; row < rows; row++) {
        for (int o = 0; o < OC; o++) {
            float value = with_bias ? bias[o] : 0.0f;
            for (int i = 0; i < C; i++) value += inp[row * C + i] * weight[(size_t)o * C + i];
            expected[row * OC + o] = value;
        }
    }
    matmul_forward(out + 1, inp, weight, with_bias ? bias : NULL, B, T, C, OC);
    assert(out[0] == 12345.0f && out[n + 1] == 12345.0f);
    assert(memcmp(out + 1, expected, n * sizeof(float)) == 0);
    free(expected); free(out); free(bias); free(weight); free(inp);
}

static void *caller(void *arg) {
    int id = (int)(intptr_t)arg;
    for (int round = 0; round < 20; round++) {
        check_case(1 + id % 2, 1 + round % 3, 127 + id, 513 + round, round % 2);
        check_case(1, 2, 3, 5, round % 2);
    }
    return NULL;
}

int main(void) {
    for (int repeat = 0; repeat < 12; repeat++) {
        check_case(2, 3, 127, 517, repeat % 2);
        check_case(1, 1, 128, 513, repeat % 2);
        check_case(1, 1, 3, 1, repeat % 2);
    }
    pthread_t callers[4];
    for (int i = 0; i < 4; i++) assert(pthread_create(&callers[i], NULL, caller, (void *)(intptr_t)i) == 0);
    for (int i = 0; i < 4; i++) assert(pthread_join(callers[i], NULL) == 0);
    puts("PASS: 196 exact scalar comparisons; tails/bias/no-bias/guards; four concurrent callers; clean pool shutdown");
    return 0;
}
