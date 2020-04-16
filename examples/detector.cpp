#include "darknet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "calibration.h"
static int coco_ids[] = {1,2,3,4,5,6,7,8,9,10,11,13,14,15,16,17,18,19,20,21,22,23,24,25,27,28,31,32,33,34,35,36,37,38,39,40,41,42,43,44,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,67,70,72,73,74,75,76,77,78,79,80,81,82,84,85,86,87,88,89,90};


void train_detector(char *datacfg, char *cfgfile, char *weightfile, int *gpus, int ngpus, int clear)
{
    list *options = read_data_cfg(datacfg);
    char *train_images = option_find_str(options, "train", "data/train.list");
    char *backup_directory = option_find_str(options, "backup", "/backup/");

    srand(time(0));
    char *base = basecfg(cfgfile);
    printf("%s\n", base);
    float avg_loss = -1;
    network **nets = (network**)calloc(ngpus, sizeof(network));

    srand(time(0));
    int seed = rand();
    int i;
    for(i = 0; i < ngpus; ++i){
        srand(seed);
#ifdef GPU
        cuda_set_device(gpus[i]);
#endif
        nets[i] = load_network(cfgfile, weightfile, clear);
        nets[i]->learning_rate *= ngpus;
    }
    srand(time(0));
    network *net = nets[0];

    int imgs = net->batch * net->subdivisions * ngpus;
    printf("Learning Rate: %g, Momentum: %g, Decay: %g\n", net->learning_rate, net->momentum, net->decay);
    data train, buffer;

    layer l = net->layers[net->n - 1];

    int classes = l.classes;
    float jitter = l.jitter;

    list *plist = get_paths(train_images);
    //int N = plist->size;
    char **paths = (char **)list_to_array(plist);

    load_args args = get_base_args(net);
    args.coords = l.coords;
    args.paths = paths;
    args.n = imgs;
    args.m = plist->size;
    args.classes = classes;
    args.jitter = jitter;
    args.num_boxes = l.max_boxes;
    args.d = &buffer;
    args.type = DETECTION_DATA;
    //args.type = INSTANCE_DATA;
    args.threads = 64;

    pthread_t load_thread = load_data(args);
    double time;
    int count = 0;
    //while(i*imgs < N*120){
    while(get_current_batch(net) < net->max_batches){
        if(l.random && count++%10 == 0){
            printf("Resizing\n");
            int dim = (rand() % 10 + 10) * 32;
            if (get_current_batch(net)+200 > net->max_batches) dim = 608;
            //int dim = (rand() % 4 + 16) * 32;
            printf("%d\n", dim);
            args.w = dim;
            args.h = dim;

            pthread_join(load_thread, 0);
            train = buffer;
            free_data(train);
            load_thread = load_data(args);

            #pragma omp parallel for
            for(i = 0; i < ngpus; ++i){
                resize_network(nets[i], dim, dim);
            }
            net = nets[0];
        }
        time=what_time_is_it_now();
        pthread_join(load_thread, 0);
        train = buffer;
        load_thread = load_data(args);

        /*
           int k;
           for(k = 0; k < l.max_boxes; ++k){
           box b = float_to_box(train.y.vals[10] + 1 + k*5);
           if(!b.x) break;
           printf("loaded: %f %f %f %f\n", b.x, b.y, b.w, b.h);
           }
         */
        /*
           int zz;
           for(zz = 0; zz < train.X.cols; ++zz){
           image im = float_to_image(net->w, net->h, 3, train.X.vals[zz]);
           int k;
           for(k = 0; k < l.max_boxes; ++k){
           box b = float_to_box(train.y.vals[zz] + k*5, 1);
           printf("%f %f %f %f\n", b.x, b.y, b.w, b.h);
           draw_bbox(im, b, 1, 1,0,0);
           }
           show_image(im, "truth11");
           cvWaitKey(0);
           save_image(im, "truth11");
           }
         */

        printf("Loaded: %lf seconds\n", what_time_is_it_now()-time);

        time=what_time_is_it_now();
        float loss = 0;
#ifdef GPU
        if(ngpus == 1){
            loss = train_network(net, train);
        } else {
            loss = train_networks(nets, ngpus, train, 4);
        }
#else
        loss = train_network(net, train);
#endif
        if (avg_loss < 0) avg_loss = loss;
        avg_loss = avg_loss*.9 + loss*.1;

        i = get_current_batch(net);
        printf("%ld: %f, %f avg, %f rate, %lf seconds, %d images\n", get_current_batch(net), loss, avg_loss, get_current_rate(net), what_time_is_it_now()-time, i*imgs);
        if(i%100==0){
#ifdef GPU
            if(ngpus != 1) sync_nets(nets, ngpus, 0);
#endif
            char buff[256];
            sprintf(buff, "%s/%s.backup", backup_directory, base);
            save_weights(net, buff);
        }
        if(i%10000==0 || (i < 1000 && i%100 == 0)){
#ifdef GPU
            if(ngpus != 1) sync_nets(nets, ngpus, 0);
#endif
            char buff[256];
            sprintf(buff, "%s/%s_%d.weights", backup_directory, base, i);
            save_weights(net, buff);
        }
        free_data(train);
    }
#ifdef GPU
    if(ngpus != 1) sync_nets(nets, ngpus, 0);
#endif
    char buff[256];
    sprintf(buff, "%s/%s_final.weights", backup_directory, base);
    save_weights(net, buff);
}


static int get_coco_image_id(char *filename)
{
    char *p = strrchr(filename, '/');
    char *c = strrchr(filename, '_');
    if(c) p = c;
    return atoi(p+1);
}

static void print_cocos(FILE *fp, char *image_path, detection *dets, int num_boxes, int classes, int w, int h)
{
    int i, j;
    int image_id = get_coco_image_id(image_path);
    for(i = 0; i < num_boxes; ++i){
        float xmin = dets[i].bbox.x - dets[i].bbox.w/2.;
        float xmax = dets[i].bbox.x + dets[i].bbox.w/2.;
        float ymin = dets[i].bbox.y - dets[i].bbox.h/2.;
        float ymax = dets[i].bbox.y + dets[i].bbox.h/2.;

        if (xmin < 0) xmin = 0;
        if (ymin < 0) ymin = 0;
        if (xmax > w) xmax = w;
        if (ymax > h) ymax = h;

        float bx = xmin;
        float by = ymin;
        float bw = xmax - xmin;
        float bh = ymax - ymin;

        for(j = 0; j < classes; ++j){
            if (dets[i].prob[j]) fprintf(fp, "{\"image_id\":%d, \"category_id\":%d, \"bbox\":[%f, %f, %f, %f], \"score\":%f},\n", image_id, coco_ids[j], bx, by, bw, bh, dets[i].prob[j]);
        }
    }
}

void print_detector_detections(FILE **fps, char *id, detection *dets, int total, int classes, int w, int h)
{
    int i, j;
    for(i = 0; i < total; ++i){
        float xmin = dets[i].bbox.x - dets[i].bbox.w/2. + 1;
        float xmax = dets[i].bbox.x + dets[i].bbox.w/2. + 1;
        float ymin = dets[i].bbox.y - dets[i].bbox.h/2. + 1;
        float ymax = dets[i].bbox.y + dets[i].bbox.h/2. + 1;

        if (xmin < 1) xmin = 1;
        if (ymin < 1) ymin = 1;
        if (xmax > w) xmax = w;
        if (ymax > h) ymax = h;

        for(j = 0; j < classes; ++j){
            if (dets[i].prob[j]) fprintf(fps[j], "%s %f %f %f %f %f\n", id, dets[i].prob[j],
                    xmin, ymin, xmax, ymax);
        }
    }
}

float my_print_detector_detections(FILE **fps, char *id, detection *dets, int total, int classes, int w, int h)
{
    int i, j;
    float mean_det=0;
    for(i = 0; i < total; ++i){
        float xmin = dets[i].bbox.x - dets[i].bbox.w/2. + 1;
        float xmax = dets[i].bbox.x + dets[i].bbox.w/2. + 1;
        float ymin = dets[i].bbox.y - dets[i].bbox.h/2. + 1;
        float ymax = dets[i].bbox.y + dets[i].bbox.h/2. + 1;

        if (xmin < 1) xmin = 1;
        if (ymin < 1) ymin = 1;
        if (xmax > w) xmax = w;
        if (ymax > h) ymax = h;

        for(j = 0; j < classes; ++j){
            mean_det+=dets[i].prob[j];
            if (dets[i].prob[j]) fprintf(fps[j], "%s %f %f %f %f %f\n", id, dets[i].prob[j],
                    xmin, ymin, xmax, ymax);
        }
    }
    printf("total:%d classes:%d mean_det:%f\n",total,classes,mean_det);
    return mean_det/classes;
}
void print_imagenet_detections(FILE *fp, int id, detection *dets, int total, int classes, int w, int h)
{
    int i, j;
    for(i = 0; i < total; ++i){
        float xmin = dets[i].bbox.x - dets[i].bbox.w/2.;
        float xmax = dets[i].bbox.x + dets[i].bbox.w/2.;
        float ymin = dets[i].bbox.y - dets[i].bbox.h/2.;
        float ymax = dets[i].bbox.y + dets[i].bbox.h/2.;

        if (xmin < 0) xmin = 0;
        if (ymin < 0) ymin = 0;
        if (xmax > w) xmax = w;
        if (ymax > h) ymax = h;

        for(j = 0; j < classes; ++j){
            int cls = j;
            if (dets[i].prob[cls]) fprintf(fp, "%d %d %f %f %f %f %f\n", id, j+1, dets[i].prob[cls],
                    xmin, ymin, xmax, ymax);
        }
    }
}

void validate_detector_flip(char *datacfg, char *cfgfile, char *weightfile, char *outfile)
{
    int j;
    list *options = read_data_cfg(datacfg);
    char *valid_images = option_find_str(options, "valid", "data/train.list");
    char *name_list = option_find_str(options, "names", "data/names.list");
    char *prefix = option_find_str(options, "results", "results");
    char **names = get_labels(name_list);
    char *mapf = option_find_str(options, "map", 0);
    int *map = 0;
    if (mapf) map = read_map(mapf);

    network *net = load_network(cfgfile, weightfile, 0);
    set_batch_network(net, 2);
    fprintf(stderr, "Learning Rate: %g, Momentum: %g, Decay: %g\n", net->learning_rate, net->momentum, net->decay);
    srand(time(0));

    list *plist = get_paths(valid_images);
    char **paths = (char **)list_to_array(plist);

    layer l = net->layers[net->n-1];
    int classes = l.classes;

    char buff[1024];
    char *type = option_find_str(options, "eval", "voc");
    FILE *fp = 0;
    FILE **fps = 0;
    int coco = 0;
    int imagenet = 0;
    if(0==strcmp(type, "coco")){
        if(!outfile) outfile = "coco_results";
        snprintf(buff, 1024, "%s/%s.json", prefix, outfile);
        fp = fopen(buff, "w");
        fprintf(fp, "[\n");
        coco = 1;
    } else if(0==strcmp(type, "imagenet")){
        if(!outfile) outfile = "imagenet-detection";
        snprintf(buff, 1024, "%s/%s.txt", prefix, outfile);
        fp = fopen(buff, "w");
        imagenet = 1;
        classes = 200;
    } else {
        if(!outfile) outfile = "comp4_det_test_";
        fps = (FILE**)calloc(classes, sizeof(FILE *));
        for(j = 0; j < classes; ++j){
            snprintf(buff, 1024, "%s/%s%s.txt", prefix, outfile, names[j]);
            fps[j] = fopen(buff, "w");
        }
    }

    int m = plist->size;
    int i=0;
    int t;

    float thresh = .005;
    float nms = .45;

    int nthreads = 4;
    image *val = (image*)(nthreads, sizeof(image));
    image *val_resized = (image*)calloc(nthreads, sizeof(image));
    image *buf = (image*)calloc(nthreads, sizeof(image));
    image *buf_resized = (image*)calloc(nthreads, sizeof(image));
    pthread_t *thr = (pthread_t*)calloc(nthreads, sizeof(pthread_t));

    image input = make_image(net->w, net->h, net->c*2);

    load_args args = {0};
    args.w = net->w;
    args.h = net->h;
    //args.type = IMAGE_DATA;
    args.type = LETTERBOX_DATA;

    for(t = 0; t < nthreads; ++t){
        args.path = paths[i+t];
        args.im = &buf[t];
        args.resized = &buf_resized[t];
        thr[t] = load_data_in_thread(args);
    }
    double start = what_time_is_it_now();
    for(i = nthreads; i < m+nthreads; i += nthreads){
        fprintf(stderr, "%d\n", i);
        for(t = 0; t < nthreads && i+t-nthreads < m; ++t){
            pthread_join(thr[t], 0);
            val[t] = buf[t];
            val_resized[t] = buf_resized[t];
        }
        for(t = 0; t < nthreads && i+t < m; ++t){
            args.path = paths[i+t];
            args.im = &buf[t];
            args.resized = &buf_resized[t];
            thr[t] = load_data_in_thread(args);
        }
        for(t = 0; t < nthreads && i+t-nthreads < m; ++t){
            char *path = paths[i+t-nthreads];
            char *id = basecfg(path);
            copy_cpu(net->w*net->h*net->c, val_resized[t].data, 1, input.data, 1);
            flip_image(val_resized[t]);
            copy_cpu(net->w*net->h*net->c, val_resized[t].data, 1, input.data + net->w*net->h*net->c, 1);

            network_predict(net, input.data);
            int w = val[t].w;
            int h = val[t].h;
            int num = 0;
            detection *dets = get_network_boxes(net, w, h, thresh, .5, map, 0, &num);
            if (nms) do_nms_sort(dets, num, classes, nms);
            if (coco){
                print_cocos(fp, path, dets, num, classes, w, h);
            } else if (imagenet){
                print_imagenet_detections(fp, i+t-nthreads+1, dets, num, classes, w, h);
            } else {
                print_detector_detections(fps, id, dets, num, classes, w, h);
            }
            free_detections(dets, num);
            free(id);
            free_image(val[t]);
            free_image(val_resized[t]);
        }
    }
    for(j = 0; j < classes; ++j){
        if(fps) fclose(fps[j]);
    }
    if(coco){
        fseek(fp, -2, SEEK_CUR); 
        fprintf(fp, "\n]\n");
        fclose(fp);
    }
    fprintf(stderr, "Total Detection Time: %f Seconds\n", what_time_is_it_now() - start);
}


void validate_detector(char *datacfg, char *cfgfile, char *weightfile, char *outfile)
{
    int j;
    list *options = read_data_cfg(datacfg);
    char *valid_images = option_find_str(options, "valid", "data/train.list");
    char *name_list = option_find_str(options, "names", "data/names.list");
    char *prefix = option_find_str(options, "results", "results");
    char **names = get_labels(name_list);
    char *mapf = option_find_str(options, "map", 0);
    int *map = 0;
    if (mapf) map = read_map(mapf);

    network *net = load_network(cfgfile, weightfile, 0);
    set_batch_network(net, 1);
    fprintf(stderr, "Learning Rate: %g, Momentum: %g, Decay: %g\n", net->learning_rate, net->momentum, net->decay);
    srand(time(0));

    list *plist = get_paths(valid_images);
    char **paths = (char **)list_to_array(plist);

    layer l = net->layers[net->n-1];
    int classes = l.classes;

    char buff[1024];
    char *type = option_find_str(options, "eval", "voc");
    FILE *fp = 0;
    FILE **fps = 0;
    int coco = 0;
    int imagenet = 0;
    if(0==strcmp(type, "coco")){
        if(!outfile) outfile = "coco_results";
        snprintf(buff, 1024, "%s/%s.json", prefix, outfile);
        fp = fopen(buff, "w");
        fprintf(fp, "[\n");
        coco = 1;
    } else if(0==strcmp(type, "imagenet")){
        if(!outfile) outfile = "imagenet-detection";
        snprintf(buff, 1024, "%s/%s.txt", prefix, outfile);
        fp = fopen(buff, "w");
        imagenet = 1;
        classes = 200;
    } else {
        if(!outfile) outfile = "";
        fps = (FILE **)calloc(classes, sizeof(FILE *));
        for(j = 0; j < classes; ++j){
            snprintf(buff, 1024, "%s/%s%s.txt", prefix, outfile, names[j]);
            fps[j] = fopen(buff, "w");
        }
    }


    int m = plist->size;
    int i=0;
    int t;

    float thresh = .005;
    float nms = .45;

    int nthreads = 4;
    image *val = (image*)calloc(nthreads, sizeof(image));
    image *val_resized = (image*)calloc(nthreads, sizeof(image));
    image *buf = (image*)calloc(nthreads, sizeof(image));
    image *buf_resized = (image*)calloc(nthreads, sizeof(image));
    pthread_t *thr = (pthread_t*)calloc(nthreads, sizeof(pthread_t));

    load_args args = {0};
    args.w = net->w;
    args.h = net->h;
    //args.type = IMAGE_DATA;
    args.type = LETTERBOX_DATA;

    for(t = 0; t < nthreads; ++t){
        args.path = paths[i+t];
        args.im = &buf[t];
        args.resized = &buf_resized[t];
        thr[t] = load_data_in_thread(args);
    }
    double start = what_time_is_it_now();
    for(i = nthreads; i < m+nthreads; i += nthreads){
        fprintf(stderr, "%d\n", i);
        for(t = 0; t < nthreads && i+t-nthreads < m; ++t){
            pthread_join(thr[t], 0);
            val[t] = buf[t];
            val_resized[t] = buf_resized[t];
        }
        for(t = 0; t < nthreads && i+t < m; ++t){
            args.path = paths[i+t];
            args.im = &buf[t];
            args.resized = &buf_resized[t];
            thr[t] = load_data_in_thread(args);
        }
        for(t = 0; t < nthreads && i+t-nthreads < m; ++t){
            char *path = paths[i+t-nthreads];
            char *id = basecfg(path);
            float *X = val_resized[t].data;
            network_predict(net, X);
            int w = val[t].w;
            int h = val[t].h;
            int nboxes = 0;
            detection *dets = get_network_boxes(net, w, h, thresh, .5, map, 0, &nboxes);
            if (nms) do_nms_sort(dets, nboxes, classes, nms);
            if (coco){
                print_cocos(fp, path, dets, nboxes, classes, w, h);
            } else if (imagenet){
                print_imagenet_detections(fp, i+t-nthreads+1, dets, nboxes, classes, w, h);
            } else {
                print_detector_detections(fps, id, dets, nboxes, classes, w, h);
            }
            free_detections(dets, nboxes);
            free(id);
            free_image(val[t]);
            free_image(val_resized[t]);
        }
    }
    for(j = 0; j < classes; ++j){
        if(fps) fclose(fps[j]);
    }
    if(coco){
        fseek(fp, -2, SEEK_CUR); 
        fprintf(fp, "\n]\n");
        fclose(fp);
    }
    fprintf(stderr, "Total Detection Time: %f Seconds\n", what_time_is_it_now() - start);
}




void my_validate_detector(char *datacfg, char *cfgfile, char *weightfile, char *outfile)
{
    int j;
    list *options = read_data_cfg(datacfg);
    char *valid_images = option_find_str(options, "valid", "data/train.list");
    char *name_list = option_find_str(options, "names", "data/names.list");
    char *prefix = option_find_str(options, "results", "results");
    char **names = get_labels(name_list);
    char *mapf = option_find_str(options, "map", 0);
    int *map = 0;
    if (mapf) map = read_map(mapf);

    network *net = load_network(cfgfile, weightfile, 0);
    set_batch_network(net, 1);
    fprintf(stderr, "Learning Rate: %g, Momentum: %g, Decay: %g\n", net->learning_rate, net->momentum, net->decay);
    srand(time(0));

    list *plist = get_paths(valid_images);
    char **paths = (char **)list_to_array(plist);

    layer l = net->layers[net->n-1];
    int classes = l.classes;

    char buff[1024];
    char *type = option_find_str(options, "eval", "voc");
    FILE *fp = 0;
    FILE **fps = 0;
    int coco = 0;
    int imagenet = 0;
    if(0==strcmp(type, "coco")){
        if(!outfile) outfile = "coco_results";
        snprintf(buff, 1024, "%s/%s.json", prefix, outfile);
        fp = fopen(buff, "w");
        fprintf(fp, "[\n");
        coco = 1;
    } else if(0==strcmp(type, "imagenet")){
        if(!outfile) outfile = "imagenet-detection";
        snprintf(buff, 1024, "%s/%s.txt", prefix, outfile);
        fp = fopen(buff, "w");
        imagenet = 1;
        classes = 200;
    } else {
        if(!outfile) outfile = "comp4_det_test_";
        fps = (FILE **)calloc(classes, sizeof(FILE *));
        for(j = 0; j < classes; ++j){
            snprintf(buff, 1024, "%s/%s%s.txt", prefix, outfile, names[j]);
            fps[j] = fopen(buff, "w");
        }
    }


    int m = plist->size;
    int i=0;
    int t;
    float mean_det=0;

    float thresh = .50;
    float nms = .45;

    int nthreads = 4;
    image *val = (image*)calloc(nthreads, sizeof(image));
    image *val_resized = (image*)calloc(nthreads, sizeof(image));
    image *buf = (image*)calloc(nthreads, sizeof(image));
    image *buf_resized = (image*)calloc(nthreads, sizeof(image));
    pthread_t *thr = (pthread_t*)calloc(nthreads, sizeof(pthread_t));

    load_args args = {0};
    args.w = net->w;
    args.h = net->h;
    //args.type = IMAGE_DATA;
    args.type = LETTERBOX_DATA;

    for(t = 0; t < nthreads; ++t){
        args.path = paths[i+t];
        args.im = &buf[t];
        args.resized = &buf_resized[t];
        thr[t] = load_data_in_thread(args);
    }
    double start = what_time_is_it_now();
    for(i = nthreads; i < m+nthreads; i += nthreads){
        fprintf(stderr, "%d\n", i);
        for(t = 0; t < nthreads && i+t-nthreads < m; ++t){
            pthread_join(thr[t], 0);
            val[t] = buf[t];
            val_resized[t] = buf_resized[t];
        }
        for(t = 0; t < nthreads && i+t < m; ++t){
            args.path = paths[i+t];
            args.im = &buf[t];
            args.resized = &buf_resized[t];
            thr[t] = load_data_in_thread(args);
        }
        for(t = 0; t < nthreads && i+t-nthreads < m; ++t){
            char *path = paths[i+t-nthreads];
            char *id = basecfg(path);
            float *X = val_resized[t].data;
            network_predict(net, X);
            int w = val[t].w;
            int h = val[t].h;
            int nboxes = 0;
            detection *dets = get_network_boxes(net, w, h, thresh, .5, map, 0, &nboxes);
            if (nms) do_nms_sort(dets, nboxes, classes, nms);
            //printf("map:%d\n",map);
            if (coco){
                print_cocos(fp, path, dets, nboxes, classes, w, h);
            } else if (imagenet){
                print_imagenet_detections(fp, i+t-nthreads+1, dets, nboxes, classes, w, h);
            } else {
                 mean_det+=my_print_detector_detections(fps, id, dets, nboxes, classes, w, h);
            }
            free_detections(dets, nboxes);
            free(id);
            free_image(val[t]);
            free_image(val_resized[t]);
        }
    }
    for(j = 0; j < classes; ++j){
        if(fps) fclose(fps[j]);
    }
    if(coco){
        fseek(fp, -2, SEEK_CUR); 
        fprintf(fp, "\n]\n");
        fclose(fp);
    }
    mean_det/= m;
    printf("m:%d",m);
    fprintf(stderr, "Total Detection Time: %f Seconds with mean det %f\n", what_time_is_it_now() - start,mean_det);
}
void validate_detector_recall(char *datacfg, char *cfgfile, char *weightfile, float thresh_orig, float hier_thresh)
{
/*     network *net = load_network(cfgfile, weightfile, 0);
    srand(2222222);
    printf("the cfg to detect is %s \n",cfgfile);
    printf("the weight to detect is %s \n",weightfile);
    set_batch_network(net, 1);
    fprintf(stderr, "Learning Rate: %g, Momentum: %g, Decay: %g\n", net->learning_rate, net->momentum, net->decay); */
    srand(time(0));

    list *plist = get_paths("/home/gaoyang/benchmark/NOK/2007_val.txt");
    char **paths = (char **)list_to_array(plist);

    

    int j, k;

    int m = plist->size;
    int i=0;

    // thresh = .5;
    float iou_thresh = .1;

    int total = 0;
    int correct = 0;
    int proposals = 0;
    float avg_iou = 0;

    for(i = 0; i < m; ++i){
        char *path = paths[i];
        network *net = load_network(cfgfile, weightfile, 0);
        //srand(2222222);
        image orig = load_image_color(path, 0, 0);
        image sized = letterbox_image(orig, net->w, net->h);
        char *id = basecfg(path);
        save_image(sized, "SIZE_RECALL");
        network_predict(net, sized.data);
        int nboxes = 0;

/*         
       
        list *options = read_data_cfg(datacfg);
        char *name_list = option_find_str(options, "names", "data/names.list");
        char **names = get_labels(name_list);

        image **alphabet = load_alphabet();
        network *net = load_network(cfgfile, weightfile, 0);
        set_batch_network(net, 1);
        srand(2222222);
        image im = load_image_color(input,0,0);
        image sized = letterbox_image(im, net->w, net->h);
        //image sized = resize_image(im, net->w, net->h);
        //image sized2 = resize_max(im, net->w);
        //image sized = crop_image(sized2, -((net->w - sized2.w)/2), -((net->h - sized2.h)/2), net->w, net->h);
        //resize_network(net, sized.w, sized.h);
        layer l = net->layers[net->n-1];


        float *X = sized.data;
        network_predict(net, X);
        printf("%s: Predicted in %f seconds.\n", input, what_time_is_it_now()-time);
        int nboxes = 0;
        detection *dets = get_network_boxes(net, im.w, im.h, thresh, hier_thresh, 0, 1, &nboxes);
        printf("%d\n", nboxes);
        detection *dets = get_network_boxes(net, im.w, im.h, thresh, hier_thresh, 0, 1, &nboxes); */

        detection *dets = get_network_boxes(net, orig.w, orig.h, thresh_orig, .5, 0, 1, &nboxes);
        //if (nms) do_nms_obj(dets, nboxes, 1, nms);
        printf("the image to detect is %s \n",path);
        printf("-------------------------totally detected %d boxes\n",nboxes);

        char labelpath[4096];
        find_replace(path, "images", "labels", labelpath);
        find_replace(labelpath, "JPEGImages", "labels", labelpath);
        find_replace(labelpath, ".jpg", ".txt", labelpath);
        find_replace(labelpath, ".JPEG", ".txt", labelpath);
        find_replace(labelpath, ".png", ".txt", labelpath);

        int num_labels = 0;
        box_label *truth = read_boxes(labelpath, &num_labels);
        if(truth == NULL){
            printf("can not load image truth label\n");
            break;
        }
        for(k = 0; k < nboxes; ++k){
            //printf("the thresh value is %d\n",proposals);
            if(dets[k].objectness > thresh_orig){
                ++proposals;
				//printf("the thresh value is %d\n",proposals);
            }
        }
        for (j = 0; j < num_labels; ++j) {
            ++total;
            box t = {truth[j].x, truth[j].y, truth[j].w, truth[j].h};
            //printf("the truth path is %f  %f   %f   %f\n",truth[j].x, truth[j].y, truth[j].w, truth[j].h);
            float best_iou = 0;
            for(k = 0; k < nboxes; ++k){
                float iou = box_iou(dets[k].bbox, t);
                //printf("the objectness value is %f\n",dets[k].objectness);
                if(dets[k].objectness > thresh_orig && iou > best_iou){
                    best_iou = iou;
                    //printf("the thresh value is %f\n",dets[k].objectness);
                }
            }
            avg_iou += best_iou;
            if(best_iou > iou_thresh){
                ++correct;
                printf("the iou value is %d",correct);
            }
        }

        fprintf(stderr, "%5d %5d %5d\tRPs/Img: %.2f\tIOU: %.2f%%\tRecall:%.2f%%\n", i, correct, total, (float)proposals/(i+1), avg_iou*100/total, 100.*correct/total);
        free(id);
        free_image(orig);
        free_image(sized);
    }
}

void validate_detector_f1(char *datacfg, char *cfgfile, char *weightfile, float thresh, float hier_thresh, int quantized)
{

/*     fprintf(stderr, "Learning Rate: %g, Momentum: %g, Decay: %g\n", net->learning_rate, net->momentum, net->decay);
    srand(time(0)); */

    list *plist = get_paths("/home/artyze/BechMark/NOK/2007_val.txt");
    char **paths = (char **)list_to_array(plist);
    int flag = 0;
    network *net;

    net = load_network(cfgfile, weightfile, quantized);
    set_batch_network(net, 1);
	if(quantized > 0){
	    printf("\n\n Quantinization! \n\n");
	    //quantinization_weightsAndactivations(net);
	    quantinization_and_get_multipliers(net);
	}

    int j, k;

    int m = plist->size;
    int i=0;

    // thresh = .5;
    float iou_thresh = .1;
    float nms = .45;

    int TP_FN = 0;
    int TP_FP=0;
    int TP = 0;
    int proposals = 0;
    float avg_iou = 0;

    for(i = 0; i < m; ++i){
        //network *net = load_network(cfgfile, weightfile, 0);
        if(flag == 1){
            printf("the flag value is %d\n", flag);
            net = load_network(cfgfile, weightfile, 0);
            set_batch_network(net, 1);
        }
        char *path = paths[i];
        image orig = load_image_color(path, 0, 0);
        image sized = resize_image(orig, net->w, net->h);
        char *id = basecfg(path);
        network_predict(net, sized.data);
        int nboxes = 0;
        detection *dets = get_network_boxes(net, sized.w, sized.h, thresh, .5, 0, 1, &nboxes);
        if (nms) do_nms_obj(dets, nboxes, 1, nms);

        char labelpath[4096];
        find_replace(path, "images", "labels", labelpath);
        find_replace(labelpath, "JPEGImages", "labels", labelpath);
        find_replace(labelpath, ".jpg", ".txt", labelpath);
        find_replace(labelpath, ".JPEG", ".txt", labelpath);

        int num_labels = 0;
        box_label *truth = read_boxes(labelpath, &num_labels);
        for(k = 0; k < nboxes; ++k){
            if(dets[k].objectness > thresh){
                ++proposals;
            }
        }
        // printf("number of boxes:%d\n",nboxes);
        for(k = 0; k < nboxes; ++k){
            TP_FP++;
        }
        for (j = 0; j < num_labels; ++j) {
            ++TP_FN;
            box t = {truth[j].x, truth[j].y, truth[j].w, truth[j].h};
            // printf("%f:\n",truth[j].x);
            float best_iou = 0;
            for(k = 0; k < nboxes; ++k){
                float iou = box_iou(dets[k].bbox, t);
                if(dets[k].objectness > thresh && iou > best_iou){
                    best_iou = iou;
                }
            }
            avg_iou += best_iou;
            if(best_iou > iou_thresh){
                ++TP;
            }
        }

        fprintf(stderr, "%5d %5d %5d\tRPs/Img: %.2f\tIOU: %.2f%%\tRecall:%.2f%%\n", i, TP, TP_FN, (float)proposals/(i+1), avg_iou*100/TP_FN, 100.*TP/TP_FN);
        fprintf(stderr, "%5d %5d %5d\tRPs/Img: %.2f\tIOU: %.2f%%\tPrecision:%.2f%%\n", i, TP, TP_FP, (float)proposals/(i+1), avg_iou*100/TP_FN, 100.*TP/TP_FP);
        fprintf(stderr, "%5d %5d %5d\tRPs/Img: %.2f\tIOU: %.2f%%\tF1:%.2f%%\n\n", i, TP, (TP_FN+TP_FP)/2, (float)proposals/(i+1), avg_iou*100/TP_FN, 100.*2*TP/(TP_FP+TP_FN));
        free(id);
        free_image(orig);
        free_image(sized);
    }
}

void detect_nok(char *datacfg, char *cfgfile, char *weightfile, float thresh, float hier_thresh)
{
    network *net = load_network(cfgfile, weightfile, 0);
    set_batch_network(net, 1);
    fprintf(stderr, "Learning Rate: %g, Momentum: %g, Decay: %g\n", net->learning_rate, net->momentum, net->decay);
    srand(time(0));

    list *options = read_data_cfg(datacfg);
    char *name_list = option_find_str(options, "names", "data/names.list");
    char **names = get_labels(name_list);
    image **alphabet = load_alphabet();

    layer l = net->layers[net->n-1];
    int j;
    int i=0;

    // float thresh = .001;
    float nms = .5;
    // make_window("predictions", orig.w, orig.h, 0);
    for(i = 1000; i < 25000; ++i){
        char *path= (char*)malloc(50);
        sprintf(path, "/home/gaoyang/nok_test/testData/%d.jpg", i);
        
        image origRec = load_image_color(path, 0, 0);
        image orig = load_image_color(path, 0, 0);
        image sized = resize_image(orig, net->w, net->h);
        network_predict(net, sized.data);
        int nboxes = 0;
        detection *dets = get_network_boxes(net, sized.w, sized.h, thresh, hier_thresh, 0, 1, &nboxes);
        if (nms) do_nms_sort(dets, nboxes, l.classes, nms);
        draw_detections(origRec, dets, nboxes, thresh, names, alphabet, l.classes);

		int defectFlag = 0;
		int cfineFlag = 0;
        int fineFlag=0;
        int numerrFlag=0;
		for (int i = 0; i < nboxes; i++) {
            char labelstr[4096] = {0};
            int best_class = -1;
            for(j = 0; j < l.classes; ++j){
                if (dets[i].prob[j] > thresh){
                    if (best_class < 0) {
                        strcat(labelstr, names[j]);
                        best_class = j;
                    } else {
                        strcat(labelstr, ", ");
                        strcat(labelstr, names[j]);
                    }
                    printf("%s: %.0f%%\n", names[j], dets[i].prob[j]*100);
                }
            }
			if (best_class == 0) {
				printf("\nmy result defect: class:%d %s: %.0f%%", best_class, labelstr, dets[i].prob[best_class] * 100);
                
				defectFlag = 1;
				//break;
			}
            if (best_class == 1) {
				printf("\nmy result defect: class:%d %s: %.0f%%", best_class, labelstr, dets[i].prob[best_class] * 100);
				fineFlag = 1;
				//break;
			}
			if (best_class == 2) {
				printf("\nmy result numErr: class:%d %s: %.0f%%", best_class, labelstr, dets[i].prob[best_class] * 100);
				numerrFlag = 1;
			}
			if (best_class == 3) {
				cfineFlag = 1;
			}
		}

        if(cfineFlag == 0 && numerrFlag==0 && defectFlag == 0 && fineFlag == 0)
        {
            char *pathSave= (char*)malloc(50);
            printf("There is no injector!");
            sprintf(pathSave, "/home/gong/nok_test/resultOut/blank/%d", i);
            save_image(origRec, pathSave);
            free(pathSave);
        }else{
            if (cfineFlag == 0) {
                printf("There is no seal ring!");

            }
            if (defectFlag == 0 && numerrFlag==0 && cfineFlag == 1) {
                char *pathSave= (char*)malloc(50);
                sprintf(pathSave, "/home/gong/nok_test/resultOut/fine/%d", i);
                save_image(origRec, pathSave);
                free(pathSave);
            }
            if(defectFlag == 1){
                char *pathSave= (char*)malloc(50);
                sprintf(pathSave, "/home/gong/nok_test/resultOut/defect/%d", i);
                save_image(origRec, pathSave);
                free(pathSave);
                char *pathSave2= (char*)malloc(50);
                sprintf(pathSave2, "/home/gong/nok_test/resultOut/defectNolabel/%d", i);
                save_image(orig, pathSave2);
                free(pathSave2);
            }
            if(cfineFlag == 0){
                char *pathSave= (char*)malloc(50);
                sprintf(pathSave, "/home/gong/nok_test/resultOut/noRing/%d", i);
                save_image(origRec, pathSave);
                free(pathSave);
                char *pathSave2= (char*)malloc(50);
                sprintf(pathSave2, "/home/gong/nok_test/resultOut/defectNolabel/%d", i);
                save_image(orig, pathSave2);
                free(pathSave2);
            }
            if(numerrFlag == 1){
                char *pathSave= (char*)malloc(50);
                sprintf(pathSave, "/home/gong/nok_test/resultOut/numerr/%d", i);
                save_image(origRec, pathSave);
                free(pathSave);
                char *pathSave2= (char*)malloc(50);
                sprintf(pathSave2, "/home/gong/nok_test/resultOut/defectNolabel/%d", i);
                save_image(orig, pathSave2);
                free(pathSave2);
            }
        }

        #ifdef OPENCV
            //��ͼƬ����
            if (defectFlag == 0 && numerrFlag==0 && cfineFlag == 1) {
                image origRecsized = resize_image(origRec, orig.w/2, orig.h/2);
                show_image(origRecsized, "predictions", 1);
                free_image(origRecsized);
            }else{
                image origRecsized = resize_image(origRec, orig.w/2, orig.h/2);
                show_image(origRecsized, "predictions", 200);
                free_image(origRecsized);
            }
        #endif
        free(path);
        free_detections(dets, nboxes);
        free_image(orig);
        free_image(origRec);
        free_image(sized);
    }
}

void test_detector(char *datacfg, char *cfgfile, char *weightfile, char *filename, float thresh, float hier_thresh, char *outfile, int fullscreen, int quantized)
{
    list *options = read_data_cfg(datacfg);
    char *name_list = option_find_str(options, "names", "data/voc.names");
    char **names = get_labels(name_list);

    image **alphabet = load_alphabet();
    network *net = load_network(cfgfile, weightfile, quantized);
    set_batch_network(net, 1);
    srand(2222222);
#ifdef QUANTIZATION
    if (quantized){
        printf("\n\n Quantinization! \n\n");
        quantinization_and_get_multipliers(net);  
    }
#elif QUANTIZATION_GOOGLE
        printf("\n\n Quantinization Google! \n\n");
        quantization_weights_and_activations(net);
        printf("\n\n Quantinization Complete! \n\n");  
#endif
    double time;
    char buff[256];
    char *input = buff;
    float nms=.45;
    while(1){
        if(filename){
            strncpy(input, filename, 256);
        } else {
            printf("Enter Image Path: ");
            fflush(stdout);
            input = fgets(input, 256, stdin);
            if(!input) return;
            strtok(input, "\n");
        }
        image im = load_image_color(input,0,0);
        image sized = letterbox_image(im, net->w, net->h);
        save_image(sized, "SIZE_DETECT");
        //image sized = resize_image(im, net->w, net->h);
        //image sized2 = resize_max(im, net->w);
        //image sized = crop_image(sized2, -((net->w - sized2.w)/2), -((net->h - sized2.h)/2), net->w, net->h);
        //resize_network(net, sized.w, sized.h);
        layer l = net->layers[net->n-1];


        float *X = sized.data;
        time=what_time_is_it_now();
        network_predict(net, X);
        printf("%s: Predicted in %f seconds.\n", input, what_time_is_it_now()-time);
        int nboxes = 0;
        detection *dets = get_network_boxes(net, im.w, im.h, thresh, hier_thresh, 0, 1, &nboxes);
        //printf("%d\n", nboxes);
        //if (nms) do_nms_obj(boxes, probs, l.w*l.h*l.n, l.classes, nms);
        if (nms) do_nms_sort(dets, nboxes, l.classes, nms);
        draw_detections(im, dets, nboxes, thresh, names, alphabet, l.classes);
        free_detections(dets, nboxes);
        if(outfile){
            save_image(im, outfile);
        }
        else{
            save_image(im, "predictions");
#ifdef OPENCV
            make_window("predictions", 512, 512, 0);
            show_image(im, "predictions", 0);
#endif
        }

        free_image(im);
        free_image(sized);
        if (filename) break;
    }
}

void run_detector(int argc, char **argv)
{
    float thresh = find_float_arg(argc, argv, "-thresh", .5);
    float hier_thresh = find_float_arg(argc, argv, "-hier", .5);
    if(argc < 4){
        fprintf(stderr, "usage: %s %s [train/test/valid] [cfg] [weights (optional)]\n", argv[0], argv[1]);
        return;
    }
    char *gpu_list = find_char_arg(argc, argv, "-gpus", 0);
    char *outfile = find_char_arg(argc, argv, "-out", 0);
    int *gpus = 0;
    int gpu = 0;
    int ngpus = 0;
    if(gpu_list){
        printf("%s\n", gpu_list);
        int len = strlen(gpu_list);
        ngpus = 1;
        int i;
        for(i = 0; i < len; ++i){
            if (gpu_list[i] == ',') ++ngpus;
        }
        gpus = (int*)calloc(ngpus, sizeof(int));
        for(i = 0; i < ngpus; ++i){
            gpus[i] = atoi(gpu_list);
            gpu_list = strchr(gpu_list, ',')+1;
        }
    } else {
        gpu = gpu_index;
        gpus = &gpu;
        ngpus = 1;
    }

    int clear = find_arg(argc, argv, "-clear");
    int fullscreen = find_arg(argc, argv, "-fullscreen");
    int quantized = find_arg(argc, argv, "-quantized");
    int calibrate_round = find_int_arg(argc, argv, "-calibrate_round", 0);
    char *datacfg = argv[3];
    char *cfg = argv[4];
    char *weights = (argc > 5) ? argv[5] : 0;
    char *filename = (argc > 6) ? argv[6]: 0;
    if(0==strcmp(argv[2], "test")) test_detector(datacfg, cfg, weights, filename, thresh, hier_thresh, outfile, fullscreen, quantized);
    else if(0==strcmp(argv[2], "train")) train_detector(datacfg, cfg, weights, gpus, ngpus, clear);
    else if(0==strcmp(argv[2], "valid")) validate_detector(datacfg, cfg, weights, outfile);
    else if(0==strcmp(argv[2], "valid2")) validate_detector_flip(datacfg, cfg, weights, outfile);
    else if(0==strcmp(argv[2], "myvalid")) my_validate_detector(datacfg, cfg, weights, outfile);
    else if(0==strcmp(argv[2], "recall")) validate_detector_recall(datacfg, cfg, weights, thresh, hier_thresh);
    else if(0==strcmp(argv[2], "detect_nok")) detect_nok(datacfg, cfg, weights, thresh, hier_thresh);
    else if(0==strcmp(argv[2], "f1")) validate_detector_f1(datacfg, cfg, weights, thresh, hier_thresh, quantized);
    else if (0 == strcmp(argv[2], "calibrate")) validate_calibrate_valid(datacfg, cfg, weights, calibrate_round);
}
