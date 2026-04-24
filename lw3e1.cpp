#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#define N 100000000LL
#define STUDENT_TICKET 431417
#define BLOCK_SIZE (10LL * STUDENT_TICKET)  // 4 314 170

// Выравнивание структуры на 64 байта (размер кэш-линии) предотвращает эффект False Sharing,
// когда разные потоки пишут в соседние переменные и заставляют процессор сбрасывать кэш.
__declspec(align(64)) struct ThreadData {
    int id;
    HANDLE event_done;      // Событие: "я закончил вычисления"
    long long start;        // Начало блока
    long long end;          // Конец блока
    double local_sum;       // Локальный результат блока
    volatile int stop;      // Флаг завершения работы потока
};

// Функция рабочего потока
DWORD WINAPI WorkerThread(LPVOID param) {
    ThreadData* data = (ThreadData*)param;

    while (1) {
        if (data->stop) break;

        double local_sum = 0.0;
        long long i = data->start;
        
        // Оптимизированный цикл: шаг 2 избавляет от if внутри горячего цикла
        for (; i + 1 < data->end; i += 2) {
            local_sum += 1.0 / (2.0 * i + 1.0);             // Четная итерация (+)
            local_sum -= 1.0 / (2.0 * (i + 1) + 1.0);       // Нечетная итерация (-)
        }
        
        // Обработка последнего элемента, если размер блока нечетный
        if (i < data->end) {
            local_sum += 1.0 / (2.0 * i + 1.0);
        }

        data->local_sum = local_sum;

        // Сигнализируем главному потоку, что блок готов
        SetEvent(data->event_done);

        // Приостанавливаем сами себя до получения следующего блока (требование задания)
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
    if (num_threads < 1) {
        fprintf(stderr, "Number of threads must be >= 1\n");
        return 1;
    }

    // Выделение памяти
    HANDLE* threads = (HANDLE*)malloc(num_threads * sizeof(HANDLE));
    HANDLE* events = (HANDLE*)malloc(num_threads * sizeof(HANDLE));
    ThreadData* thread_data = (ThreadData*)_aligned_malloc(num_threads * sizeof(ThreadData), 64);

    // Инициализация
    for (int i = 0; i < num_threads; ++i) {
        // Создаем событие с автосбросом (Auto-reset event)
        events[i] = CreateEvent(NULL, FALSE, FALSE, NULL);
        
        thread_data[i].id = i;
        thread_data[i].event_done = events[i];
        thread_data[i].stop = 0;
        thread_data[i].local_sum = 0.0;

        // Создаем потоки сразу в приостановленном состоянии
        threads[i] = CreateThread(NULL, 0, WorkerThread, &thread_data[i], CREATE_SUSPENDED, NULL);
    }

    LARGE_INTEGER freq, start_time, end_time;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start_time);

    long long current_block = 0;
    double total_sum = 0.0;
    int active_threads = 0;

    // Первичное распределение блоков
    for (int i = 0; i < num_threads && current_block < N; ++i) {
        thread_data[i].start = current_block;
        long long end = current_block + BLOCK_SIZE;
        thread_data[i].end = (end > N) ? N : end;
        current_block += BLOCK_SIZE;
        
        active_threads++;
        ResumeThread(threads[i]);
    }

    // Динамическое распределение оставшихся блоков
    while (active_threads > 0) {
        // Ждем, пока ХОТЯ БЫ ОДИН поток не завершит свой блок
        DWORD dwWait = WaitForMultipleObjects(num_threads, events, FALSE, INFINITE);
        int idx = dwWait - WAIT_OBJECT_0;

        // Собираем результат
        total_sum += thread_data[idx].local_sum;
        active_threads--;

        if (current_block < N) {
            // Если есть еще блоки, выдаем новый
            thread_data[idx].start = current_block;
            long long end = current_block + BLOCK_SIZE;
            thread_data[idx].end = (end > N) ? N : end;
            current_block += BLOCK_SIZE;

            // БЕЗОПАСНОЕ ПРОБУЖДЕНИЕ:
            // Убеждаемся, что рабочий поток действительно успел вызвать SuspendThread.
            // ResumeThread возвращает количество приостановок ДО вызова функции.
            // Если вернулся 0, значит поток еще в процессе засыпания (между SetEvent и SuspendThread).
            while (ResumeThread(threads[idx]) == 0) {
                Sleep(0); // Отдаем квант времени, ждем фактической приостановки
            }
            active_threads++;
        }
    }

    QueryPerformanceCounter(&end_time);
    double elapsed = (double)(end_time.QuadPart - start_time.QuadPart) / freq.QuadPart;

    // Корректное завершение всех потоков
    for (int i = 0; i < num_threads; ++i) {
        thread_data[i].stop = 1;
        // Будим потоки в последний раз, чтобы они увидели stop = 1 и завершили выполнение (return 0)
        while (ResumeThread(threads[i]) == 0) {
            Sleep(0);
        }
    }

    // Дожидаемся полного выхода потоков
    WaitForMultipleObjects(num_threads, threads, TRUE, INFINITE);

    double pi = 4.0 * total_sum;
    printf("pi=%.10f time=%.6f\n", pi, elapsed);

    // Освобождение ресурсов
    for (int i = 0; i < num_threads; ++i) {
        CloseHandle(events[i]);
        CloseHandle(threads[i]);
    }
    free(threads);
    free(events);
    _aligned_free(thread_data);

    return 0;
}