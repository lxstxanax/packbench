#include "motor_dc.h"
#include "tim.h"

#define MOTOR_MAX 5000

// Скорость нарастания. MotorDC_Update() вызывается из прерывания TIM4
// каждые 2 мс, поэтому 10 единиц за вызов дают 0 -> MOTOR_MAX примерно
// за 1 секунду (5000 / 10 = 500 вызовов = 1000 мс).
// Разгон: MotorDC_Update() вызывается из прерывания TIM4 каждые 2 мс.
// 5 единиц за вызов дают 0 -> DC_PWM_MAX примерно за 2 секунды
// (5000 / 5 = 1000 вызовов = 2000 мс). При ограничителе 40% реальный
// верх достигается примерно за 1.1 с. Увеличить число = резче трогание.
#define MOTOR_RAMP_STEP              5

// Пауза на нуле при смене направления. Сначала спускаемся до нуля,
// ждём, и только потом разгоняемся в другую сторону. Если этого не
// делать, мост прикладывает питание против противо-ЭДС вращающегося
// мотора, и ток в обмотке доходит примерно до двойного пускового.
// Именно на этом срабатывает защита пакета.
#define MOTOR_REVERSE_DWELL_TICKS  150   // 150 * 2 мс = 300 мс

static volatile int16_t  motor_target  = 0;  // куда едем
static volatile int16_t  motor_current = 0;  // что реально записано в CCR
static volatile uint16_t motor_dwell   = 0;  // остаток паузы на нуле

// Запись пары CCR. Два важных момента:
//
// 1) Канал, который должен обнулиться, пишется ПЕРВЫМ. Тогда любое
//    промежуточное состояние -- это "меньше тяги", но никогда не
//    "оба канала активны".
// 2) Пара пишется под запретом прерываний с сохранением PRIMASK:
//    функция вызывается и из main, и из обработчика TIM4. Преднагрузка
//    CCR у TIM1 включена (HAL_TIM_PWM_ConfigChannel), поэтому обе
//    записи попадают в один период ШИМ, если между ними не влезло
//    прерывание.
static void MotorDC_ApplyRaw(int16_t speed)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    if (speed > 0)
    {
        // ВПЕРЁД
        __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_2, 0);
        __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, (uint32_t)speed);
    }
    else if (speed < 0)
    {
        // НАЗАД
        __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0);
        __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_2, (uint32_t)(-speed));
    }
    else
    {
        // СТОП. Оба канала в нуле -- оба нижних ключа открыты, мотор
        // замкнут накоротко через мост. Это торможение, а не выбег.
        __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0);
        __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_2, 0);
    }

    __set_PRIMASK(primask);
}

void MotorDC_Init(void)
{
    motor_target  = 0;
    motor_current = 0;
    motor_dwell   = 0;

    MotorDC_ApplyRaw(0);
}

// Задать ЦЕЛЕВУЮ скорость. Значение в CCR меняется постепенно, в
// MotorDC_Update(). Ступенек по току больше нет ни на одном пути:
// ни из UART, ни из самотеста.
void MotorDC_SetSpeed(int speed)
{
    // ограничение
    if (speed > MOTOR_MAX) speed = MOTOR_MAX;
    if (speed < -MOTOR_MAX) speed = -MOTOR_MAX;

    motor_target = (int16_t)speed;
}

// Немедленный сброс тяги, без рампы. Рампа нужна на разгоне, а не на
// остановке: переход в ноль замыкает обмотку на мост, ток гасится
// внутри моста и из пакета при этом ничего не тянется.
void MotorDC_Stop(void)
{
    motor_target  = 0;
    motor_current = 0;
    motor_dwell   = 0;

    MotorDC_ApplyRaw(0);
}

int16_t MotorDC_GetSpeed(void)
{
    return motor_current;
}

// Вызывается из обработчика TIM4 каждые 2 мс, рядом со Steering_Update().
void MotorDC_Update(void)
{
    int16_t target  = motor_target;
    int16_t current = motor_current;

    // Смена направления: пока не дошли до нуля, целью считаем ноль.
    if (((target > 0) && (current < 0)) ||
        ((target < 0) && (current > 0)))
    {
        target = 0;
    }

    // Пауза на нуле перед разгоном в обратную сторону.
    if ((current == 0) && (motor_dwell != 0U))
    {
        motor_dwell--;
        return;
    }

    if (current < target)
    {
        current = ((target - current) > MOTOR_RAMP_STEP) ?
                  (int16_t)(current + MOTOR_RAMP_STEP) : target;
    }
    else if (current > target)
    {
        current = ((current - target) > MOTOR_RAMP_STEP) ?
                  (int16_t)(current - MOTOR_RAMP_STEP) : target;
    }
    else
    {
        return;
    }

    // Дошли до нуля, а цель -- другое направление: взводим паузу.
    if ((current == 0) && (motor_target != 0))
    {
        motor_dwell = MOTOR_REVERSE_DWELL_TICKS;
    }

    motor_current = current;
    MotorDC_ApplyRaw(current);
}
