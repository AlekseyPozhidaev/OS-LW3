#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define N 100000000LL
#define BLOCK_SIZE (4314170)

typedef struct {
    int id;
    HANDLE event_done;
    long long start;
    long long end;
    volatile int stop;
    volatile BOOL ready_to_sleep;
    CRITICAL_SECTION* cs_suspend;
} ThreadData;

DWORD WINAPI WorkerThread(LPVOID param) {
    ThreadData* data = (ThreadData*)param;
    double local_sum;

    while (1) {
        local_sum = 0.0;
        for (long long i = data->start; i < data->end; ++i) {
            double term = 1.0 / (2.0 * i + 1.0);
            if (i & 1)
                local_sum -= term;
            else
                local_sum += term;
        }

        extern CRITICAL_SECTION cs_sum;
        EnterCriticalSection(&cs_sum);
        extern double total_sum;
        total_sum += local_sum;
        LeaveCriticalSection(&cs_sum);

        SetEvent(data->event_done);

        EnterCriticalSection(data->cs_suspend);
        data->ready_to_sleep = TRUE;
        LeaveCriticalSection(data->cs_suspend);

        SuspendThread(GetCurrentThread());

        EnterCriticalSection(data->cs_suspend);
        data->ready_to_sleep = FALSE;
        LeaveCriticalSection(data->cs_suspend);

        // Проверка на завершение
        if (data->stop) break;
    }
    return 0;
}

CRITICAL_SECTION cs_sum;
double total_sum = 0.0;

int main(int argc, char* argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <num_threads>\n", argv[0]);
        return 1;
    }
    int num_threads = atoi(argv[1]);
    if (num_threads < 1) {
        fprintf(stderr, "Number of threads must be positive.\n");
        return 1;
    }

    InitializeCriticalSection(&cs_sum);
    CRITICAL_SECTION cs_suspend;
    InitializeCriticalSection(&cs_suspend);

    HANDLE* threads = (HANDLE*)malloc(num_threads * sizeof(HANDLE));
    ThreadData* thread_data = (ThreadData*)malloc(num_threads * sizeof(ThreadData));
    HANDLE* events = (HANDLE*)malloc(num_threads * sizeof(HANDLE));
    if (!threads || !thread_data || !events) {
        fprintf(stderr, "Memory allocation failed.\n");
        DeleteCriticalSection(&cs_sum);
        DeleteCriticalSection(&cs_suspend);
        return 1;
    }

    for (int i = 0; i < num_threads; ++i) {
        events[i] = CreateEvent(NULL, FALSE, FALSE, NULL);
        if (!events[i]) {
            fprintf(stderr, "CreateEvent failed for thread %d\n", i);
            // очистка
            for (int j = 0; j < i; ++j) CloseHandle(events[j]);
            free(threads); free(thread_data); free(events);
            DeleteCriticalSection(&cs_sum);
            DeleteCriticalSection(&cs_suspend);
            return 1;
        }
        thread_data[i].id = i;
        thread_data[i].event_done = events[i];
        thread_data[i].start = 0;
        thread_data[i].end = 0;
        thread_data[i].stop = 0;
        thread_data[i].ready_to_sleep = FALSE;
        thread_data[i].cs_suspend = &cs_suspend;

        threads[i] = CreateThread(NULL, 0, WorkerThread, &thread_data[i],
                                  CREATE_SUSPENDED, NULL);
        if (!threads[i]) {
            fprintf(stderr, "CreateThread failed for thread %d\n", i);
            for (int j = 0; j <= i; ++j) {
                if (events[j]) CloseHandle(events[j]);
                if (threads[j]) CloseHandle(threads[j]);
            }
            free(threads); free(thread_data); free(events);
            DeleteCriticalSection(&cs_sum);
            DeleteCriticalSection(&cs_suspend);
            return 1;
        }
    }

    total_sum = 0.0;
    long long next_block_start = 0;

    for (int i = 0; i < num_threads && next_block_start < N; ++i) {
        thread_data[i].start = next_block_start;
        thread_data[i].end = next_block_start + BLOCK_SIZE;
        if (thread_data[i].end > N) thread_data[i].end = N;
        next_block_start += BLOCK_SIZE;
        ResumeThread(threads[i]);   // запустить поток
    }

    LARGE_INTEGER freq, start_time, end_time;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start_time);

    while (next_block_start < N) {
        DWORD idx = WaitForMultipleObjects(num_threads, events, FALSE, INFINITE);
        if (idx == WAIT_FAILED) {
            fprintf(stderr, "WaitForMultipleObjects failed\n");
            break;
        }
        idx -= WAIT_OBJECT_0;

        while (1) {
            EnterCriticalSection(&cs_suspend);
            BOOL ready = thread_data[idx].ready_to_sleep;
            LeaveCriticalSection(&cs_suspend);
            if (ready) break;
            Sleep(0);
        }

        thread_data[idx].start = next_block_start;
        thread_data[idx].end = next_block_start + BLOCK_SIZE;
        if (thread_data[idx].end > N) thread_data[idx].end = N;
        next_block_start += BLOCK_SIZE;

        ResumeThread(threads[idx]);
    }

    for (int i = 0; i < num_threads; ++i) {
        WaitForSingleObject(events[i], INFINITE);
    }

    QueryPerformanceCounter(&end_time);
    double elapsed = (double)(end_time.QuadPart - start_time.QuadPart) / freq.QuadPart;

    
    for (int i = 0; i < num_threads; ++i) {
        while (1) {
            EnterCriticalSection(&cs_suspend);
            BOOL ready = thread_data[i].ready_to_sleep;
            LeaveCriticalSection(&cs_suspend);
            if (ready) break;
            Sleep(0);
        }
        thread_data[i].stop = 1;
        ResumeThread(threads[i]);
    }

    WaitForMultipleObjects(num_threads, threads, TRUE, INFINITE);

    double pi = 4.0 * total_sum;
    printf("pi=%.10f time=%.6f\n", pi, elapsed);

    for (int i = 0; i < num_threads; ++i) {
        CloseHandle(threads[i]);
        CloseHandle(events[i]);
    }
    free(threads);
    free(thread_data);
    free(events);
    DeleteCriticalSection(&cs_sum);
    DeleteCriticalSection(&cs_suspend);

    return 0;
}