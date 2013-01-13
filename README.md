# pit - Pipe multiplexing Tool [![Build Status](https://secure.travis-ci.org/avz/pit.png)](http://travis-ci.org/avz/pit)
## Overview

Утилита позволяет создать поток данных с неограниченным количеством писателей и читателей.
Это может быть использовано, например, для реалтайм генерации и обработки данных в несколько потоков.
Дополнительные потоки чтения или записи можно добавлять в любой момент.

Кроме того, если читатели обрабатывают данные медленее, чем они появляются, то всё, что не успевает обрабатываться
будет поставлено в очередь и будет обрабатываться постепенно. Таким образом, процесс-писатель не будет ждать обработки
и данные не потеряются.

![Overview](http://share.nologin.ru/img/overview600.png)

Утилита ```pit`` работает в двух режимах: режим записи в поток (``pit -w``), в котором все данные,
получаемые из ``STDIN`` помещаются в буфер в ФС и режим чтения (``pit -r``), который достаёт данные из буфера
и отдаёт их в ``STDOUT``. При этом, потоки записи и чтения абсолютно не зависят друг от друга, так что можно
в любой момент добавить или убрать несколько писателей или читателей.

## Пример
Для примера возьмём вариант с картинки выше: 2 писателя и 3 читателя.
Создадим в фоне 2 процесса - читателя и будем записывать в общий поток данные из ``/dev/random``
```
% pit -w /tmp/randomStream < /dev/random &
% pit -w /tmp/randomStream < /dev/random &
```
Здесь, опция ``-w`` означает, что нам нужно писать в поток, а ``/tmp/randomStream`` - каталог потока,
который будет создан для хранения чанков данных.

Далее, создаём трёх читателей:
```
% pit -r /tmp/randomStream >> random &
% pit -r /tmp/randomStream >> random &
% pit -r /tmp/randomStream >> random &
```

Опция ``-r`` - чтение из потока в каталоге ``/tmp/randomStream``.

Всё, теперь данные, читаемые в 2 потока из ``/dev/random`` разбиваются на куски и сохраняются в ``random`` в 3 потока.
Читатели завершат работу как только обработают все накопленые в потоке данные и при условии,
что в этот поток больше никто не пишет (нет процессов ``pit -w`` в этом же потоке).

Если нужно сделать так, чтобы читатели ждали поступления новых данных, даже если в поток в настоящее
время никто не пишет (то есть нет процессов-писателей), то можно воспользоваться опцией ``-p``:
```
% pit -pr /tmp/randomStream >> random
```

В этом случае чтение завершится при удалении каталога с потоком:
```
% rm -rf /tmp/randomStream
```

## Использование
```
% pit -w [ -s bytes ][ -t seconds ][-b] /path/to/storage/dir
% pit -r [-pW] /path/to/storage/dir
```

``/path/to/storage/dir`` - путь, по которому будет создан каталог с данными.
На момент запуска не должен существовать

 * ``-w`` работать в режиме записи на диск
   * ``-s bytes`` примерный размер файла данных (чанка) при сохранении. При достижении лимита будет создан новый файл. По умолчанию - 1MiB (1024 * 1024 байт)
   * ``-t seconds`` создавать новый файл данных примерно раз в ``seconds`` секунд. По умолчанию 1 секунда
   * ``-b`` режим, при котором входной поток считается неструктурированным и граница чанка может быть в любом месте
 * ``-r`` работать в режиме чтения с диска
   * ``-W`` ожидать появления каталога с потоком, если он ещё не создан
   * ``-p`` включит persistent mode. В этом режиме читатель не завершает работу после полной обработки, а ждёт появления нового писателя. Читатель завершит работу только если каталог с потоком будет удалён. Так же включает в себя опцию ``-W``

## Установка

```
# git clone https://github.com/avz/pit.git
# cd pit
# sudo make install
```

или однострочник
```
# cd /tmp && git clone https://github.com/avz/pit.git && cd pit && sudo make install
```

## Балансировка

Принцип распределения даннх между читателями основан на разделении поступающего потока на небольшие куски (чанки),
каждый чанк отдаётся на обработку одному читателю, после полной обработки читатель ищет следующий не занятый чанк.

Для примера возьмём 2 потока записи и 3 потока чтения.

Первый писатель записывает:
```
writer 1 line 1
writer 1 line 2
writer 1 line 3
writer 1 line 4
```

Второй
```
writer 2 line 1
writer 2 line 2
writer 2 line 3
writer 2 line 4
```

Принципы разделения на чанки следующие:
  - все данные в одном чанке гарантированно были записаны одним писателем
  - чанк может обрываться только после символа ``\n`` или если писатель завершился

Второй принцип гарантирует, что читатель получит записанную строку целиком, это позволяет организовать
полноценную передачу сообщений от писателя к читателю, оформляя эти сообщения в виде отдельных строк.

Размер чанка примерно равен величине заданной опцией ``-s``, но может быть немного больше или немного меньше
(+- длина самой большой строки). Кроме того, на создание новых чанков влияет опция ``-t``, форсирущая
создание новых чанков каждые ``N`` секунд для более эффективной балансировки между читателями.

Таким образом, чанки поделятся примерно так

Чанк 1:
```
writer 1 line 1
writer 1 line 2
```

Чанк 2:
```
writer 2 line 1
writer 2 line 2
```

Чанк 3:
```
writer 2 line 3
writer 2 line 4
```

Чанк 4
```
writer 1 line 3
writer 1 line 4
```

Нумерация чанков основана на времени начала записи, так что на практике она может отличаться. При выборе чанка читатель
основывается на двух правилах:
  - выбранный чанк не должен иметь другого читателя
  - чанк должен иметь самую раннюю дату создания

Таким образом, чанки распределяются примерно так
```
Читатель 1 - чанк 1
Читатель 2 - чанк 2
Читатель 3 - чанк 3
Читатель 2 - чанк 4
```

Реальный порядок может сильно отичаться, он зависит от времени, потраченного на обработку каждого конкретного чанка, от
времени запуска новых читателей, от планировщика процессов ОК. В общем, порядок обработки чанков может быть любым,
но в среднем она происходит в порядке записи.
