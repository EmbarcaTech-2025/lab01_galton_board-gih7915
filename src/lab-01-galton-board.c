#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "pico/binary_info.h"
#include "inc/ssd1306.h"
#include "hardware/i2c.h"
#include "hardware/adc.h"

// Configurações de hardware
const uint I2C_SDA_PIN = 14;
const uint I2C_SCL_PIN = 15;
#define BUTTON_A_PIN 5

// Constantes do display
#define DISPLAY_WIDTH 128
#define DISPLAY_HEIGHT 64
#define DISPLAY_PAGES (DISPLAY_HEIGHT / 8)

// Estrutura para representar uma bola
typedef struct {
    int speed;                  // Velocidade de queda (1-3)
    int x_left, x_right;        // Posição horizontal (dois pixels)
    int y;                      // Posição vertical
    uint8_t pattern;            // Padrão de bits para desenho
    int channel;                // Canaleta onde caiu
    int registered;             // Se já foi registrada
    int collision_dir;          // Direção após colisão (0=esq, 1=dir)
    int prev_page;              // Página anterior
    int page_changed;           // Flag de mudança de página
    int active;                 // Se está ativa/em movimento
} Ball;

// Estrutura para representar uma canaleta
typedef struct {
    int start_col;              // Coluna inicial
    int end_col;                // Coluna final
    int current_col;            // Coluna atual para desenho
    int *bit_positions;         // Posições dos bits para desenho
    int ball_count;             // Contador de bolas
} Channel;

// Variáveis globais
Channel *channels = NULL;
int total_channels = 0;
int distribution_view = 0;

// Protótipos de funções
void init_hardware();
void init_balls(Ball *balls, int count);
void init_channels(uint8_t *buffer);
void draw_pins(uint8_t *buffer);
void update_ball(Ball *ball, uint8_t *buffer);
void draw_ball(Ball *ball, uint8_t *buffer);
void erase_ball(Ball *ball, uint8_t *buffer);
int check_collision(Ball *ball, uint8_t *buffer);
void move_ball_x(Ball *ball, uint8_t *buffer, int direction);
void move_ball_y(Ball *ball, uint8_t *buffer);
void register_ball(Ball *ball, uint8_t *buffer);
void show_distribution(uint8_t *buffer);
void reset_simulation(Ball *balls, int count, uint8_t *buffer);
bool handle_button(uint8_t *buffer, Ball *balls, int count, int *current_ball);

int main() {
    stdio_init_all();
    
    // Inicializa hardware
    init_hardware();
    
    // Configura área de renderização
    struct render_area display_area = {
        start_column: 0,
        end_column: DISPLAY_WIDTH - 1,
        start_page: 0,
        end_page: DISPLAY_PAGES - 1
    };
    calculate_render_area_buffer_length(&display_area);
    
    // Buffer do display
    uint8_t display_buffer[ssd1306_buffer_length];
    memset(display_buffer, 0, sizeof(display_buffer));
    
    // Inicializa componentes
    init_channels(display_buffer);
    draw_pins(display_buffer);
    render_on_display(display_buffer, &display_area);
    
    // Configura bolas
    const int TOTAL_BALLS = 1000;
    Ball balls[TOTAL_BALLS];
    init_balls(balls, TOTAL_BALLS);
    
    int current_ball = 0;
    
    // Loop principal
    while (true) {
        if (current_ball >= TOTAL_BALLS) {
            if (handle_button(display_buffer, balls, TOTAL_BALLS, &current_ball)) {
                continue;
            }
            sleep_ms(10);
            continue;
        }
        
        Ball *ball = &balls[current_ball % TOTAL_BALLS];
        
        if (ball->registered) {
            current_ball++;
            continue;
        }
        
        update_ball(ball, display_buffer);
        render_on_display(display_buffer, &display_area);
        current_ball++;
        
        // Pequena pausa para controlar velocidade
        sleep_ms(5);
    }
}

// Implementações das funções

void init_hardware() {
    // ADC para números aleatórios
    adc_init();
    adc_select_input(3);
    
    // I2C para o display
    i2c_init(i2c1, 400 * 1000);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_PIN);
    gpio_pull_up(I2C_SCL_PIN);
    
    // Botão
    gpio_init(BUTTON_A_PIN);
    gpio_set_dir(BUTTON_A_PIN, GPIO_IN);
    gpio_pull_up(BUTTON_A_PIN);
    
    // Display OLED
    ssd1306_init();
}

void init_balls(Ball *balls, int count) {
    for (int i = 0; i < count; i++) {
        balls[i] = (Ball){
            .speed = 1,         // Velocidade mais lenta
            .x_left = (DISPLAY_WIDTH/2)-1,
            .x_right = DISPLAY_WIDTH/2,
            .y = 0,
            .pattern = 0x03,    // Dois pixels lado a lado
            .channel = -1,
            .registered = 0,
            .collision_dir = 1,
            .prev_page = 0,
            .page_changed = 0,
            .active = 1
        };
    }
}

void init_channels(uint8_t *buffer) {
    int page = (DISPLAY_HEIGHT - 8) / 8;
    int channel_width = 8;
    
    // Calcula quantas canaletas cabem
    total_channels = ((DISPLAY_WIDTH * 3/4) - (DISPLAY_WIDTH/4)) / channel_width;
    channels = malloc(total_channels * sizeof(Channel));
    
    // Cria canaletas igualmente espaçadas
    for (int i = 0; i < total_channels; i++) {
        int x = (DISPLAY_WIDTH/4) + 4 + (i * channel_width);
        channels[i] = (Channel){
            .start_col = x,
            .end_col = x + channel_width,
            .current_col = (x > DISPLAY_WIDTH/2) ? x : x + channel_width,
            .ball_count = 0
        };
        
        // Aloca array de posições de bits
        int size = channels[i].end_col - channels[i].start_col;
        channels[i].bit_positions = malloc(size * sizeof(int));
        memset(channels[i].bit_positions, 0, size * sizeof(int));
        
        // Desenha haste da canaleta
        buffer[x + page * DISPLAY_WIDTH] = 0xFF;
    }
}

void draw_pins(uint8_t *buffer) {
    int pin_count = 0;
    int start_x = DISPLAY_WIDTH / 2;
    
    for (int y = 8; y < DISPLAY_HEIGHT - 8; y += 8) {
        pin_count++;
        for (int j = 0; j < pin_count; j++) {
            int x = start_x + (j * 8);
            int page = y / 8;
            uint8_t mask = 1 << (y % 8);
            buffer[x + page * DISPLAY_WIDTH] |= mask;
        }
        start_x -= 4;  // Ajusta para formar triângulo
    }
}

void update_ball(Ball *ball, uint8_t *buffer) {
    if (!ball->active) {
        if (ball->channel == -1) {
            // Determina canaleta
            for (int i = 0; i < total_channels; i++) {
                if (ball->x_left >= channels[i].start_col && 
                    ball->x_right <= channels[i].end_col) {
                    ball->channel = i;
                    channels[i].ball_count++;
                    break;
                }
            }
        }
        register_ball(ball, buffer);
        return;
    }
    
    if (check_collision(ball, buffer)) {
        move_ball_x(ball, buffer, adc_read() & 0x01);  // Direção aleatória
    }
    move_ball_y(ball, buffer);
}

int check_collision(Ball *ball, uint8_t *buffer) {
    // Verifica colisão com pinos
    int page = ball->y / 8;
    int next_page = (ball->y + 8) / 8;
    uint8_t mask = ball->pattern << ball->speed;
    uint8_t next_mask = ball->pattern >> (8 - ball->speed);
    
    return (buffer[ball->x_left + page * DISPLAY_WIDTH] & mask) ||
           (buffer[ball->x_right + page * DISPLAY_WIDTH] & mask) ||
           (buffer[ball->x_left + next_page * DISPLAY_WIDTH] & next_mask) ||
           (buffer[ball->x_right + next_page * DISPLAY_WIDTH] & next_mask);
}

void move_ball_x(Ball *ball, uint8_t *buffer, int direction) {
    erase_ball(ball, buffer);
    
    if (direction) {  // Esquerda
        ball->x_left -= 2;
        ball->x_right = ball->x_left - 1;
        ball->collision_dir = 0;
    } else {  // Direita
        ball->x_left += 2;
        ball->x_right = ball->x_left + 1;
        ball->collision_dir = 1;
    }
    
    draw_ball(ball, buffer);
}

void move_ball_y(Ball *ball, uint8_t *buffer) {
    erase_ball(ball, buffer);
    
    if ((ball->pattern >> (8 - ball->speed)) > 0) {
        // Muda de página
        ball->pattern = ball->pattern >> (8 - ball->speed);
        ball->prev_page = ball->y;
        ball->y += 8;
        ball->page_changed = 1;
    } else {
        // Continua na mesma página
        if (ball->page_changed) {
            // Limpa rastro da página anterior
            buffer[ball->x_left + (ball->prev_page/8) * DISPLAY_WIDTH] &= ~(ball->pattern << (8 - ball->speed));
            buffer[ball->x_right + (ball->prev_page/8) * DISPLAY_WIDTH] &= ~(ball->pattern << (8 - ball->speed));
            ball->page_changed = 0;
        }
        ball->pattern = ball->pattern << ball->speed;
    }
    
    // Verifica se chegou na base
    if (ball->y >= DISPLAY_HEIGHT - 8) {
        ball->active = 0;
        return;
    }
    
    draw_ball(ball, buffer);
}

void draw_ball(Ball *ball, uint8_t *buffer) {
    int page = ball->y / 8;
    buffer[ball->x_left + page * DISPLAY_WIDTH] |= ball->pattern;
    buffer[ball->x_right + page * DISPLAY_WIDTH] |= ball->pattern;
}

void erase_ball(Ball *ball, uint8_t *buffer) {
    int page = ball->y / 8;
    buffer[ball->x_left + page * DISPLAY_WIDTH] &= ~ball->pattern;
    buffer[ball->x_right + page * DISPLAY_WIDTH] &= ~ball->pattern;
}

void register_ball(Ball *ball, uint8_t *buffer) {
    if (ball->channel == -1 || ball->registered) return;
    
    Channel *channel = &channels[ball->channel];
    int col = channel->current_col - channel->start_col;
    int bit_pos = channel->bit_positions[col] & 0x07;
    
    int page = (DISPLAY_HEIGHT - 1) / 8;
    buffer[channel->current_col + page * DISPLAY_WIDTH] |= (0x80 >> bit_pos);
    
    channel->bit_positions[col]++;
    ball->registered = 1;
    
    // Atualiza coluna para próxima bola
    if (channel->start_col > DISPLAY_WIDTH/2) {
        channel->current_col++;
        if (channel->current_col >= channel->end_col) {
            channel->current_col = channel->start_col + 1;
        }
    } else {
        channel->current_col--;
        if (channel->current_col <= channel->start_col) {
            channel->current_col = channel->end_col - 1;
        }
    }
}

void show_distribution(uint8_t *buffer) {
    memset(buffer, 0, ssd1306_buffer_length);
    
    // Mostra contagem
    int x = 0;
    for (int i = 0; i < total_channels; i++) {
        char count_str[10];
        snprintf(count_str, sizeof(count_str), "%d", channels[i].ball_count);
        ssd1306_draw_string(buffer, x, 0, count_str);
        x += 20;
    }
    
    // Desenha distribuição
    for (int i = 0; i < total_channels; i++) {
        Channel *channel = &channels[i];
        channel->current_col = (channel->start_col > DISPLAY_WIDTH/2) ? 
                              channel->start_col : channel->end_col - 1;
        
        for (int j = 0; j < channel->ball_count; j++) {
            int col = abs(channel->current_col - channel->start_col);
            int bit_pos = j % 8;
            int page = (DISPLAY_HEIGHT - 1 - (j / 8)) / 8;
            
            buffer[channel->current_col + page * DISPLAY_WIDTH] |= (0x80 >> bit_pos);
            
            // Atualiza coluna
            if (channel->start_col > DISPLAY_WIDTH/2) {
                channel->current_col++;
                if (channel->current_col >= channel->end_col) {
                    channel->current_col = channel->start_col + 1;
                }
            } else {
                channel->current_col--;
                if (channel->current_col <= channel->start_col) {
                    channel->current_col = channel->end_col - 1;
                }
            }
        }
    }
}

void reset_simulation(Ball *balls, int count, uint8_t *buffer) {
    // Limpa canaletas
    for (int i = 0; i < total_channels; i++) {
        free(channels[i].bit_positions);
    }
    free(channels);
    channels = NULL;
    total_channels = 0;
    
    // Reinicia bolas
    init_balls(balls, count);
    
    // Redesenha componentes
    memset(buffer, 0, ssd1306_buffer_length);
    init_channels(buffer);
    draw_pins(buffer);
    
    distribution_view = 0;
}

bool handle_button(uint8_t *buffer, Ball *balls, int count, int *current_ball) {
    if (gpio_get(BUTTON_A_PIN) == 0) {
        sleep_ms(50);  // Debounce
        if (gpio_get(BUTTON_A_PIN) == 0) {
            if (!distribution_view) {
                show_distribution(buffer);
                distribution_view = 1;
            } else {
                reset_simulation(balls, count, buffer);
                *current_ball = 0;
                distribution_view = 0;
            }
            return true;
        }
    }
    return false;
}
