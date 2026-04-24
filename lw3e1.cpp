#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#define N 100000000LL
#define STUDENT_TICKET 431417
#define BLOCK_SIZE (10LL * STUDENT_TICKET)  // 4 314 170

typedef struct {
    int id;
    HANDLE event_done;
    long long start;
    long long end;
    double local_sum;
    volatile int stop;
    volatile int is_suspending; // Флаг готовности ко сну
} ThreadData;

DWORD WINAPI WorkerThread(LPVOID param) {
    ThreadData* data = (ThreadData*)param;

    while (1) {
        if (data->stop) break;

        // В ТОЧНОСТИ как в OpenMP версии: честная математика, которую любит компилятор
        double local_sum = 0.0;
        for (long long i = data->start; i < data->end; ++i) {
            double term = (i % 2 == 0) ? 1.0 : -1.0;
            term /= (2.0 * i + 1.0);
            local_sum += term;
        }
        data->local_sum = local_sum;

        // Сигнализируем о завершении вычислений
        SetEvent(data->event_done);

        // Ставим флаг, что мы прямо сейчас уходим в SuspendThread
        data->is_suspending = 1;
        SuspendThread(GetCurrentThread());
    }
    return 0;
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <num_threads>\n", argv[0]);
        return 1;
    }

    int num_threads = atoi(argv[1]);
    if (num_threads < 1) return 1;

    HANDLE* threads = (HANDLE*)malloc(num_threads * sizeof(HANDLE));
    HANDLE* events = (HANDLE*)malloc(num_threads * sizeof(HANDLE));
    ThreadData* thread_data = (ThreadData*)malloc(num_threads * sizeof(ThreadData));

    for (int i = 0; i < num_threads; ++i) {
        events[i] = CreateEvent(NULL, FALSE, FALSE, NULL);
        thread_data[i].id = i;
        thread_data[i].event_done = events[i];
        thread_data[i].stop = 0;
        thread_data[i].is_suspending = 0;
        thread_data[i].local_sum = 0.0;

        threads[i] = CreateThread(NULL, 0, WorkerThread, &thread_data[i], CREATE_SUSPENDED, NULL);
    }

    LARGE_INTEGER freq, start_time, end_time;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start_time);

    long long current_block = 0;
    double total_sum = 0.0;
    int active_threads = 0;

    // Раздаем первые блоки
    for (int i = 0; i < num_threads && current_block < N; ++i) {
        thread_data[i].start = current_block;
        long long end = current_block + BLOCK_SIZE;
        thread_data[i].end = (end > N) ? N : end;
        current_block += BLOCK_SIZE;
        
        active_threads++;
        ResumeThread(threads[i]);
    }

    // Динамическая раздача
    while (active_threads > 0) {
        DWORD dwWait = WaitForMultipleObjects(num_threads, events, FALSE, INFINITE);
        int idx = dwWait - WAIT_OBJECT_0;

        total_sum += thread_data[idx].local_sum;
        active_threads--;

        if (current_block < N) {
            thread_data[idx].start = current_block;
            long long end = current_block + BLOCK_SIZE;
            thread_data[idx].end = (end > N) ? N : end;
            current_block += BLOCK_SIZE;

            // БЫСТРОЕ ожидание (без Sleep!), пока поток не дойдет до SuspendThread
            while (thread_data[idx].is_suspending == 0) {}
            thread_data[idx].is_suspending = 0; // Сбрасываем флаг

            // Гарантированно и быстро будим поток
            while (ResumeThread(threads[idx]) == 0) {}
            
            active_threads++;
        }
    }

    QueryPerformanceCounter(&end_time);
    double elapsed = (double)(end_time.QuadPart - start_time.QuadPart) / freq.QuadPart;

    // Правильное и безопасное завершение потоков
    for (int i = 0; i < num_threads; ++i) {
        thread_data[i].stop = 1;
        // Ждем, если поток еще не успел уснуть после последнего блока
        while (thread_data[i].is_suspending == 0) {} 
        while (ResumeThread(threads[i]) == 0) {}
    }

    WaitForMultipleObjects(num_threads, threads, TRUE, INFINITE);

    double pi = 4.0 * total_sum;
    printf("pi=%.10f time=%.6f\n", pi, elapsed);

    for (int i = 0; i < num_threads; ++i) {
        CloseHandle(events[i]);
        CloseHandle(threads[i]);
    }
    free(threads);
    free(events);
    free(thread_data);

    return 0;
}