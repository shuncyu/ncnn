
: << !

# float32 --> float16
./ncnnoptimize yolov3-tiny.param yolov3-tiny.bin yolov3-tiny_op.param yolov3-tiny_op.bin 1

# float32 --> int8
./ncnn2table --param  yolov3-tiny.param --bin yolov3-tiny.bin yolov3-tiny_int8.param yolov3-tiny --images images/ --output yolov3-tiny.table --thread 2

./ncnn2int8 yolo.param yolo.bin model.param model.bin model.table

!

# myself 

# float32 --> float16
./ncnnoptimize test/yolov3-tiny.param test/yolov3-tiny.bin test/yolov3-tiny_op.param test/yolov3-tiny_op.bin 1

# float32 --> int8
./ncnn2table --param  test/yolov3-tiny.param --bin test/yolov3-tiny.bin test/yolov3-tiny_int8.param test/yolov3-tiny --images images/ --output test/yolov3-tiny.table --thread 2

./ncnn2int8 test/yolov3-tiny.param test/yolov3-tiny.bin test/yolov3-tiny_int8.param test/yolov3-tiny_int8.bin test/yolov3-tiny.table
