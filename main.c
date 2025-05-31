#include <gtk/gtk.h>
#include <portaudio.h>
#include <sndfile.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define FRAMES_PER_BUFFER 256
#define CHANNELS 1
#define SAMPLE_FORMAT paInt16

typedef short SAMPLE;

typedef struct {
    SAMPLE buffer[FRAMES_PER_BUFFER];
    int buffer_ready;

    GtkWidget *drawing_area;
    guint timer_id;

    int sample_rate;
    gboolean recording;
    PaStream *stream;

    // Para guardar audio
    SAMPLE *recorded_samples;
    size_t recorded_samples_capacity;
    size_t recorded_samples_count;
} AppData;

static AppData app_data = {0};

// --- PortAudio callback ---
static int pa_callback(const void *inputBuffer, void *outputBuffer,
                       unsigned long framesPerBuffer,
                       const PaStreamCallbackTimeInfo* timeInfo,
                       PaStreamCallbackFlags statusFlags,
                       void *userData) {
    AppData *data = (AppData*)userData;
    const SAMPLE *in = (const SAMPLE*)inputBuffer;

    if (inputBuffer == NULL) {
        memset(data->buffer, 0, sizeof(SAMPLE)*framesPerBuffer);
    } else {
        memcpy(data->buffer, in, sizeof(SAMPLE)*framesPerBuffer);

        // Guardar para WAV si está grabando
        if (data->recording) {
            // Reservar más memoria si es necesario
            if (data->recorded_samples_count + framesPerBuffer > data->recorded_samples_capacity) {
                size_t new_capacity = data->recorded_samples_capacity + 44100 * 10; // +10s de margen
                SAMPLE *new_buf = realloc(data->recorded_samples, new_capacity * sizeof(SAMPLE));
                if (new_buf == NULL) {
                    g_printerr("No se pudo reservar memoria para grabación.\n");
                    return paComplete;
                }
                data->recorded_samples = new_buf;
                data->recorded_samples_capacity = new_capacity;
            }

            memcpy(data->recorded_samples + data->recorded_samples_count, in, framesPerBuffer * sizeof(SAMPLE));
            data->recorded_samples_count += framesPerBuffer;
        }
    }

    data->buffer_ready = 1;

    return paContinue;
}

// --- Draw oscilloscope ---
static gboolean on_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    AppData *data = (AppData*)user_data;

    if (!data->buffer_ready) {
        return FALSE;
    }

    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);
    int width = alloc.width;
    int height = alloc.height;

    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);

    cairo_set_source_rgb(cr, 0, 1, 0); // Green waveform
    cairo_set_line_width(cr, 1.0);

    int mid_y = height / 2;

    cairo_move_to(cr, 0, mid_y);

    for (int i = 0; i < FRAMES_PER_BUFFER; i++) {
        // Normalize sample to [-1,1]
        double sample = data->buffer[i] / 32768.0;
        int x = (int)((double)i / FRAMES_PER_BUFFER * width);
        int y = mid_y - (int)(sample * (mid_y - 10));
        cairo_line_to(cr, x, y);
    }
    cairo_stroke(cr);

    // Draw RMS (volume) bar
    double rms = 0.0;
    for (int i = 0; i < FRAMES_PER_BUFFER; i++) {
        double s = data->buffer[i] / 32768.0;
        rms += s * s;
    }
    rms = sqrt(rms / FRAMES_PER_BUFFER);

    // Draw volume bar at bottom
    int bar_width = (int)(rms * width);
    cairo_set_source_rgb(cr, 1, 0, 0); // Red bar
    cairo_rectangle(cr, 0, height - 20, bar_width, 15);
    cairo_fill(cr);

    return FALSE;
}

// --- Timer callback to update oscilloscope ---
static gboolean update_oscilloscope(gpointer user_data) {
    GtkWidget *drawing_area = ((AppData*)user_data)->drawing_area;
    gtk_widget_queue_draw(drawing_area);
    return TRUE; // continue calling
}

// --- Start recording ---
static gboolean start_recording(AppData *data) {
    PaError err;

    if (data->recording)
        return FALSE;

    err = Pa_Initialize();
    if (err != paNoError) {
        g_printerr("Error inicializando PortAudio: %s\n", Pa_GetErrorText(err));
        return FALSE;
    }

    err = Pa_OpenDefaultStream(&data->stream,
                               CHANNELS,
                               0,
                               SAMPLE_FORMAT,
                               data->sample_rate,
                               FRAMES_PER_BUFFER,
                               pa_callback,
                               data);
    if (err != paNoError) {
        g_printerr("Error abriendo stream: %s\n", Pa_GetErrorText(err));
        Pa_Terminate();
        return FALSE;
    }

    // Preparar buffer de grabación
    free(data->recorded_samples);
    data->recorded_samples = NULL;
    data->recorded_samples_capacity = 0;
    data->recorded_samples_count = 0;

    err = Pa_StartStream(data->stream);
    if (err != paNoError) {
        g_printerr("Error iniciando stream: %s\n", Pa_GetErrorText(err));
        Pa_CloseStream(data->stream);
        Pa_Terminate();
        return FALSE;
    }

    data->recording = TRUE;
    return TRUE;
}

// --- Stop recording ---
static void stop_recording(AppData *data) {
    if (!data->recording)
        return;

    Pa_StopStream(data->stream);
    Pa_CloseStream(data->stream);
    Pa_Terminate();
    data->recording = FALSE;
}

// --- Guardar WAV ---
static void save_wav(AppData *data) {
    if (data->recorded_samples_count == 0) {
        g_print("No hay datos para guardar.\n");
        return;
    }

    // Crear nombre archivo con timestamp
    time_t t = time(NULL);
    struct tm *tm_info = localtime(&t);
    char filename[128];
    strftime(filename, sizeof(filename), "grabacion_%Y%m%d_%H%M%S.wav", tm_info);

    SF_INFO sfinfo;
    memset(&sfinfo, 0, sizeof(sfinfo));
    sfinfo.samplerate = data->sample_rate;
    sfinfo.channels = CHANNELS;
    sfinfo.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

    SNDFILE *outfile = sf_open(filename, SFM_WRITE, &sfinfo);
    if (!outfile) {
        g_printerr("No se pudo abrir archivo WAV para escritura.\n");
        return;
    }

    sf_count_t written = sf_write_short(outfile, data->recorded_samples, data->recorded_samples_count);
    if (written != (sf_count_t)data->recorded_samples_count) {
        g_printerr("Error escribiendo archivo WAV.\n");
    } else {
        g_print("Archivo guardado: %s\n", filename);
    }

    sf_close(outfile);
}

// --- Botón iniciar/parar ---
static void on_button_record_toggled(GtkToggleButton *toggle_button, gpointer user_data) {
    AppData *data = (AppData*)user_data;

    if (gtk_toggle_button_get_active(toggle_button)) {
        if (!start_recording(data)) {
            gtk_toggle_button_set_active(toggle_button, FALSE);
            g_printerr("No se pudo iniciar grabación.\n");
        } else {
            gtk_button_set_label(GTK_BUTTON(toggle_button), "Detener grabación");
        }
    } else {
        stop_recording(data);
        save_wav(data);
        gtk_button_set_label(GTK_BUTTON(toggle_button), "Iniciar grabación");
    }
}

// --- Callback para cambio de frecuencia ---
static void on_freq_scale_value_changed(GtkRange *range, gpointer user_data) {
    AppData *data = (AppData*)user_data;
    int new_rate = (int)gtk_range_get_value(range);
    data->sample_rate = new_rate;

    // Si está grabando, reinicia stream con nueva frecuencia
    if (data->recording) {
        stop_recording(data);
        start_recording(data);
    }

    GtkWidget *label = g_object_get_data(G_OBJECT(range), "freq_label");
    char buf[64];
    snprintf(buf, sizeof(buf), "Frecuencia: %d Hz", new_rate);
    gtk_label_set_text(GTK_LABEL(label), buf);
}

// --- Ventana cerrar ---
static void on_window_destroy(GtkWidget *widget, gpointer user_data) {
    AppData *data = (AppData*)user_data;
    stop_recording(data);
    free(data->recorded_samples);
    gtk_main_quit();
}

// --- Main ---
int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    app_data.sample_rate = 20000;

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Osciloscopio y Grabación Ultrasónica");
    gtk_window_set_default_size(GTK_WINDOW(window), 700, 350);
    g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), &app_data);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    // Slider frecuencia
    GtkWidget *freq_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 20000, 90000, 1000);
    gtk_scale_set_draw_value(GTK_SCALE(freq_scale), FALSE);
    gtk_box_pack_start(GTK_BOX(vbox), freq_scale, FALSE, FALSE, 0);

    GtkWidget *freq_label = gtk_label_new("Frecuencia: 20000 Hz");
    gtk_box_pack_start(GTK_BOX(vbox), freq_label, FALSE, FALSE, 0);

    g_object_set_data(G_OBJECT(freq_scale), "freq_label", freq_label);
    g_signal_connect(freq_scale, "value-changed", G_CALLBACK(on_freq_scale_value_changed), &app_data);

    // Área dibujo osciloscopio
    app_data.drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(app_data.drawing_area, 700, 220);
    gtk_box_pack_start(GTK_BOX(vbox), app_data.drawing_area, TRUE, TRUE, 0);
    g_signal_connect(app_data.drawing_area, "draw", G_CALLBACK(on_draw), &app_data);

    // Botón iniciar/parar grabación
    GtkWidget *btn_record = gtk_toggle_button_new_with_label("Iniciar grabación");
    gtk_box_pack_start(GTK_BOX(vbox), btn_record, FALSE, FALSE, 0);
    g_signal_connect(btn_record, "toggled", G_CALLBACK(on_button_record_toggled), &app_data);

    // Timer para actualizar osciloscopio
    app_data.timer_id = g_timeout_add(33, update_oscilloscope, &app_data);

    gtk_widget_show_all(window);
    gtk_main();

    if (app_data.timer_id)
        g_source_remove(app_data.timer_id);

    free(app_data.recorded_samples);

    return 0;
}
