// Copyright 2024 Robert Bosch GmbH
//
// SPDX-License-Identifier: Apache-2.0

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <time.h>
#include <dse/clib/util/strings.h>
#include <dse/clib/util/yaml.h>
#include <dse/modelc/runtime.h>
#include <dse/platform.h>
#include <dse/logger.h>


#define GENERAL_BUFFER_LEN 255
#define STEP_SIZE          MODEL_DEFAULT_STEP_SIZE
#define END_TIME           3600


static void __log(const char* format, ...)
{
    printf("Runtime: ");
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}


static void __log_operating_conditions(void)
{
    char      general_buffer[GENERAL_BUFFER_LEN];
    time_t    rawtime;
    struct tm t_info;

    __log("Version: %s", MODELC_VERSION);
    __log("Platform: %s-%s", PLATFORM_OS, PLATFORM_ARCH);
    time(&rawtime);
#ifdef _WIN32
    gmtime_s(&t_info, &rawtime);
#else
    gmtime_r(&rawtime, &t_info);
#endif
    __log("Time: %.4d-%-.2d-%.2d %.2d:%.2d:%.2d (UTC)", 1900 + t_info.tm_year,
        t_info.tm_mon, t_info.tm_mday, t_info.tm_hour, t_info.tm_min,
        t_info.tm_sec);
#ifndef _WIN32
    gethostname(general_buffer, GENERAL_BUFFER_LEN - 1);
    __log("Host: %s", general_buffer);
#endif
    getcwd(general_buffer, GENERAL_BUFFER_LEN - 1);
    __log("CWD: %s", general_buffer);
}


static int __build_args(
    RuntimeModelDesc* rm, const char* sim_yaml, int* argc, char*** argv)
{
    /* Extract parameters from the simulation YAML file. */
    YamlDocList* doc_list = dse_yaml_load_file(sim_yaml, NULL);
    YamlNode*    a_node = dse_yaml_find_node_in_doclist(
           doc_list, "Stack", "metadata/annotations");
    size_t       yaml_len = 0;
    const char** yaml_files =
        dse_yaml_get_array(a_node, "model_runtime__yaml_files", &yaml_len);
    const char* model_inst =
        dse_yaml_get_scalar(a_node, "model_runtime__model_inst");
    size_t model_inst_opt_len = strlen(model_inst) + 8;
    char*  model_inst_opt = calloc(model_inst_opt_len, sizeof(char));
    snprintf(model_inst_opt, model_inst_opt_len, "--name=%s", model_inst);

    /* Allocate ARGV. */
    *argc = 2 + yaml_len;
    char** _argv = calloc(*argc, sizeof(char*));
    *argv = _argv;

    /* Build the ARGV. */
    _argv[0] = strdup("model_runtime");
    _argv[1] = model_inst_opt;
    for (size_t i = 0; i < yaml_len; i++) {
        _argv[2 + i] = dse_path_cat(rm->runtime.sim_path, yaml_files[i]);
    }

    dse_yaml_destroy_doc_list(doc_list);
    free(yaml_files);
    return 0;
}


static void __free_args(int argc, char** argv)
{
    /* Call at end of simulation as memory may be referenced. */
    for (int i = 0; i < argc; i++)
        free(argv[i]);
    free(argv);
}


RuntimeModelDesc* model_runtime_create(RuntimeModelDesc* rm)
{
    /* Calculate and log operating conditions. */
    char* simulation_yaml =
        dse_path_cat(rm->runtime.sim_path, rm->runtime.simulation_yaml);
    __log_operating_conditions();
    __log("Simulation Path: %s", rm->runtime.sim_path);
    __log("Simulation YAML: %s", simulation_yaml);
    __log("Model: %s", rm->runtime.model_name);

    /* Argument parsing. */
    __build_args(rm, simulation_yaml, &rm->runtime.argc, &rm->runtime.argv);
    ModelCArguments args;
    modelc_set_default_args(&args, NULL,
        rm->runtime.step_size ? rm->runtime.step_size : STEP_SIZE,
        rm->runtime.end_time ? rm->runtime.end_time : END_TIME);
    modelc_parse_arguments(
        &args, rm->runtime.argc, rm->runtime.argv, "Model Loader and Stepper");
    if (args.name == NULL) log_fatal("name argument not provided!");
    if (rm->runtime.log_level) args.log_level = rm->runtime.log_level;
    free(simulation_yaml);
    simulation_yaml = NULL;

    /* Configure the Model (takes config from parsed YAML docs). */
    int rc = modelc_configure(&args, rm->model.sim);
    if (rc) exit(rc);
    ModelInstanceSpec* mi = modelc_get_model_instance(rm->model.sim, args.name);
    if (mi == NULL) log_fatal("ModelInstance %s not found!", args.name);

    /* Loading the Model library and symbols. */
    char* model_filename =
        dse_path_cat(rm->runtime.sim_path, mi->model_definition.full_path);
    __log("Loading model: %s ...", model_filename);
    void* handle = dlopen(model_filename, RTLD_NOW | RTLD_LOCAL);
    free(model_filename);
    if (handle == NULL) {
        log_error("ERROR: dlopen call failed: %s", dlerror());
        log_fatal("Model library not loaded!");
    }
    ModelVTable vtable;
    vtable.create = dlsym(handle, MODEL_CREATE_FUNC_NAME);
    vtable.step = dlsym(handle, MODEL_STEP_FUNC_NAME);
    vtable.destroy = dlsym(handle, MODEL_DESTROY_FUNC_NAME);
    __log("vtable.create: %p", vtable.create);
    __log("vtable.step: %p", vtable.step);
    __log("vtable.destroy: %p", vtable.destroy);
    if (vtable.create == NULL && vtable.step == NULL) {
        log_fatal("vtable not complete!");
    }

    /* Call the create function of the Model. */
    rc = modelc_model_create(rm->model.sim, mi, &vtable);
    if (rc) {
        log_fatal("Call: model_setup_func failed! (rc=%d)!", rc);
    }
    __log("mi->model_desc->vtable.create: %p", mi->model_desc->vtable.create);
    __log("mi->model_desc->vtable.step: %p", mi->model_desc->vtable.step);
    __log("mi->model_desc->vtable.destroy: %p", mi->model_desc->vtable.destroy);

    /* Complete the RuntimeModelDesc object .*/
    memcpy(&rm->model.vtable, &vtable, sizeof(ModelVTable));
    rm->model.index = mi->model_desc->index;
    rm->model.mi = mi;
    rm->model.sv = mi->model_desc->sv;

    return rm;
}


int model_runtime_step(
    RuntimeModelDesc* rm, double* model_time, double stop_time)
{
    int    rc = 0;
    double _step_epsilon = rm->model.sim->step_size * 0.01;
    double _model_time = *model_time;
    double model_current_time;
    double model_stop_time;
    do {
        /* Determine times. */
        model_current_time = _model_time;
        model_stop_time = stop_time;

        if (model_current_time >= model_stop_time) {
            break;
        }

        /*  Use the simulation step size.
         *  Increment via Kahan summation.
         *
         *  model_stop_time = model_time + sim->step_size;
         */
        double y = rm->model.sim->step_size - rm->runtime.step_time_correction;
        double t = model_current_time + y;
        rm->runtime.step_time_correction = (t - model_current_time) - y;
        model_stop_time = t;

        /* Model stop time past Simulation stop time. */
        if (model_stop_time > stop_time + _step_epsilon) break;

        /*  Step the model(s). This will call:
         *      controller_step()   // Sets AdapterModel `stop_time` according
         *                          // to `sim->step_size`.
         *      sim_step_models()   // Iterates all MIs contained in the sim.
         *      step_model()        // Sets AdapterModel `model_time`.
         */
        log_trace("model_runtime_step: model_time=%f, stop_time=%f",
            model_current_time, model_stop_time);
        rc |= modelc_sync(rm->model.sim);
        _model_time = _model_time + rm->model.sim->step_size;
    } while (model_stop_time + _step_epsilon < stop_time);

    *model_time = stop_time;
    return rc;
}


void model_runtime_destroy(RuntimeModelDesc* rm)
{
    dse_yaml_destroy_doc_list(rm->model.mi->yaml_doc_list);
    modelc_exit(rm->model.sim);
    __free_args(rm->runtime.argc, rm->runtime.argv);
}
