//
//   Copyright (C) 2010 Andy Pugh
//
//   This program is free software; you can redistribute it and/or modify
//   it under the terms of the GNU General Public License as published by
//   the Free Software Foundation; either version 2 of the License, or
//   (at your option) any later version.
//
//   This program is distributed in the hope that it will be useful,
//   but WITHOUT ANY WARRANTY; without even the implied warranty of
//   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//   GNU General Public License for more details.
//
//   You should have received a copy of the GNU General Public License
//   along with this program; if not, write to the Free Software
//   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
//

// FIXME: Since the change to auto-configuring cards, the separate drivers
// for 8i20 and 7i64 make no sense. They should simply spoof a GTOC and share a
// structure with the auto-conf type. It probably makes sense to consolidate
// into one file too, though it will be a biggie. 


#include <linux/slab.h>

#include "rtapi.h"
#include "rtapi_app.h"
#include "rtapi_string.h"
#include "rtapi_math.h"

#include "hal.h"

#include "hal/drivers/mesa-hostmot2/hostmot2.h"

//utility function delarations
int hm2_sserial_stopstart(hostmot2_t *hm2, hm2_module_descriptor_t *md, 
                          hm2_sserial_instance_t *inst, u32 start_mode);


int hm2_sserial_parse_md(hostmot2_t *hm2, int md_index) {
    hm2_module_descriptor_t *md = &hm2->md[md_index];
    int i, c;
    int pin = -1;
    int port_pin, port;
    u32 ddr_reg, src_reg, addr, buff;
    int r = 0;
    int count = 0;
    int chan_counts[] = {0,0,0,0,0,0,0,0};
    
    hm2->sserial.version = md->version;

    //
    // some standard sanity checks
    //

    if (hm2_md_is_consistent(hm2, md_index, 0, 5, 0x40, 0x001F)) {
        HM2_ERR("The bitfile contains Smart Serial modules for a firmware "
                "revision < rev22. This Driver now requires rev22 or newer "
                "firmwares\n");
        return -EINVAL;
    }
    
    if (!hm2_md_is_consistent_or_complain(hm2, md_index, 0, 6, 0x40, 0x003C)) {
        HM2_ERR("inconsistent Module Descriptor!\n");
        return -EINVAL;
    }

    if (hm2->sserial.num_instances != 0) {
        HM2_ERR(
                "found duplicate Module Descriptor for %s (inconsistent firmwar"
                "e), not loading driver\n",
                hm2_get_general_function_name(md->gtag)
                );
        return -EINVAL;
    }
    if (hm2->config.num_sserials > md->instances) {
        HM2_ERR(
                "num_sserials references %d instances, but only %d are available"
                ", not loading driver\n",
                hm2->config.num_sserials,
                md->instances
                );
        return -EINVAL;
    }

    if (hm2->config.num_sserials == 0) {
        return 0;
    }

    //
    // looks good, start initializing
    //

    if (hm2->config.num_sserials == -1) {
        hm2->sserial.num_instances = md->instances;
    } else {
        hm2->sserial.num_instances = hm2->config.num_sserials;
    }
    //num_instances may be revised down depending on detected hardware
    HM2_DBG("sserial_num_instances = %i\n", hm2->sserial.num_instances);

    // allocate the global interface pins into shared memory
    hm2->sserial.hal = (hm2_sserial_hal_t *)hal_malloc(sizeof(hm2_sserial_hal_t));
    if (hm2->sserial.hal == NULL) {
        HM2_ERR("hm2_sserial_parse_md: hm2_sserial_hal: out of memory!\n");
        r = -ENOMEM;
        goto fail0;
    }
    // allocate the per-instance HAL shared memory
    hm2->sserial.instance = (hm2_sserial_instance_t *)
    hal_malloc(hm2->sserial.num_instances * sizeof(hm2_sserial_instance_t));
    if (hm2->sserial.instance == NULL) {
        HM2_ERR("hm2_sserial_parse_md: hm2_sserial_instance: out of memory!\n");
        r = -ENOMEM;
        goto fail0;
    }
    // We can't create the pins until we know what is on each channel, and
    // can't communicate until the pin directions are set up.

    // Temporarily enable the pins that are not masked by sserial_mode

    for (port  = 0; port < hm2->ioport.num_instances; port ++) {
        ddr_reg = 0;
        src_reg = 0;
        for (port_pin = 0 ; port_pin < hm2->idrom.port_width; port_pin ++){
            pin++;
            if (hm2->pin[pin].sec_tag == HM2_GTAG_SMARTSERIAL) {
                // look for highest-indexed pin to determine number of channels
                if ((hm2->pin[pin].sec_pin & 0x0F) > chan_counts[hm2->pin[pin].sec_unit]) {
                    chan_counts[hm2->pin[pin].sec_unit] = (hm2->pin[pin].sec_pin & 0x0F);
                }
                // check if the channel is enabled
                HM2_DBG("sec unit = %i, sec pin = %i\n", hm2->pin[pin].sec_unit, hm2->pin[pin].sec_pin & 0x0F);
                if (hm2->config.sserial_modes[hm2->pin[pin].sec_unit]
                                        [(hm2->pin[pin].sec_pin & 0x0F) - 1] != 'x') {
                    src_reg |= (1 << port_pin);
                    if (hm2->pin[pin].sec_pin & 0x80){ ddr_reg |= (1 << port_pin); }
                }
            }
        }

        hm2->llio->write(hm2->llio, hm2->ioport.ddr_addr + 4 * port,
                         &ddr_reg, sizeof(u32));
        hm2->llio->write(hm2->llio, hm2->ioport.alt_source_addr + 4 * port,
                         &src_reg, sizeof(u32));

    }

    // Now iterate through the sserial instances, seeing what is on the enabled pins.
    for (i = 0 ; i < hm2->sserial.num_instances ; i++) {

        hm2_sserial_instance_t *inst = &hm2->sserial.instance[count];
        inst->module_index = i;
        inst->num_channels = chan_counts[i];

        HM2_DBG("num channels = %x\n", inst->num_channels);

        inst->command_reg_addr = md->base_address + i * md->instance_stride;
        inst->data_reg_addr
        = md->base_address + i * md->instance_stride + md->register_stride;
        
        buff=0x4000; //Reset
        hm2->llio->write(hm2->llio, inst->command_reg_addr, &buff, sizeof(u32));
        buff=0x0001; //Clear
        hm2->llio->write(hm2->llio, inst->command_reg_addr, &buff, sizeof(u32));
        if (hm2_sserial_waitfor(hm2, inst->command_reg_addr, 0xFFFFFFFF,1007) < 0){
            r = -EINVAL;
            goto fail0;
        }
        do {
            buff=0x2003; //Read firmware version
            hm2->llio->write(hm2->llio, inst->command_reg_addr, &buff, sizeof(u32));
            if (hm2_sserial_waitfor(hm2, inst->command_reg_addr, 0xFFFFFFFF,49) < 0){
                r = -EINVAL;
                goto fail0;
            }
            hm2->llio->read(hm2->llio, inst->data_reg_addr, &buff, sizeof(u32));
        } while (buff == 0xAA);
        HM2_PRINT("Smart Serial Firmware Version %i\n",buff);

        //start up in setup mode
        if ( hm2_sserial_stopstart(hm2, md, inst, 0xF00) < 0) {goto fail0;}
        
        inst->tag_8i20 = 0;
        inst->tag_7i64 = 0;
        inst->num_8i20 = 0;
        inst->num_7i64 = 0;
        inst->num_auto = 0;
        inst->tag_auto = 0;
        inst->tag_all = 0;
        inst->num_all = 0;
        for (c = 0 ; c < inst->num_channels ; c++) {
            u32 user1, user2;
            // user1 addr
            addr = md->base_address + 4 * md->register_stride
                                    + i * md->instance_stride + c * sizeof(u32);
            hm2->llio->read(hm2->llio, addr, &user1, sizeof(u32));
            // user2 addr
            addr = md->base_address + 5 * md->register_stride
                                    + i * md->instance_stride + c * sizeof(u32);
            hm2->llio->read(hm2->llio, addr, &user2, sizeof(u32));
            if (user2 & 0x0000ffff){// Parameter discovery
                inst->tag_auto |= (1 << c);
                inst->tag_all |= (1 << c);
                inst->num_auto += 1;
                inst->num_all += 1;
            } else {
                switch (user1) {
                    case 0x0: // nothing connected, or masked by config
                        for (pin = 0 ; pin < hm2->num_pins ; pin++){
                            if (hm2->pin[pin].sec_tag == HM2_GTAG_SMARTSERIAL
                                && (hm2->pin[pin].sec_pin & 0x0F) - 1  == c
                                && hm2->pin[pin].sec_unit == i){
                                hm2->pin[pin].sec_tag = 0;
                            }
                        }
                        break;
                    case HM2_SSERIAL_TYPE_8I20: // 8i20 found   
                        inst->tag_8i20 |= (1 << c);
                        inst->tag_all |= (1 << c);
                        inst->num_8i20 += 1;
                        inst->num_all += 1;
                        break;
                    case HM2_SSERIAL_TYPE_7I64: // 7i64 found
                        inst->tag_7i64 |= (1 << c);
                        inst->tag_all |= (1 << c);
                        inst->num_7i64 += 1;
                        inst->num_all += 1;
                        break;
                    default:
                        HM2_ERR("Unsupported Device %s4.4 found on sserial %d "
                                "channel %d\n", (char*)&user1, i, c);
                }
            }
        }
        if (inst->num_all > 0){
            r = hal_pin_bit_newf(HAL_IN, &(inst->run),
                                 hm2->llio->comp_id, 
                                 "%s.sserial.port-%1d.run",
                                 hm2->llio->name, i);
            if (r < 0) {
                HM2_ERR("error adding pin %s.sserial.%1d.run. aborting\n",
                        hm2->llio->name, i);
                goto fail0;
            }
            r = hal_pin_u32_newf(HAL_OUT, &(inst->state),
                                 hm2->llio->comp_id, 
                                 "%s.sserial.port-%1d.port_state",
                                 hm2->llio->name, i);
            if (r < 0) {
                HM2_ERR("error adding pin %s.sserial.%1d.port_state. aborting\n",
                        hm2->llio->name, i);
                goto fail0;
            }
            r = hal_pin_u32_newf(HAL_OUT, &(inst->fault_count),
                                 hm2->llio->comp_id, 
                                 "%s.sserial.port-%1d.fault-count",
                                 hm2->llio->name, i);
            if (r < 0) {
                HM2_ERR("error adding pin %s.sserial.%1d.fault-count. aborting\n",
                        hm2->llio->name, i);
                goto fail0;
            }
            r = hal_param_u32_newf(HAL_RW, &(inst->fault_inc),
                                   hm2->llio->comp_id, 
                                   "%s.sserial.port-%1d.fault-inc",
                                   hm2->llio->name, i);
            if (r < 0) {
                HM2_ERR("error adding parameter %s.sserial.port-%1d.fault-inc"
                        " aborting\n",hm2->llio->name, i);
                goto fail0;
            }            

            r = hal_param_u32_newf(HAL_RW, &(inst->fault_dec),
                                   hm2->llio->comp_id, 
                                   "%s.sserial.port-%1d.fault-dec",
                                   hm2->llio->name, i);
            if (r < 0) {
                HM2_ERR("error adding parameter %s.sserial.port-%1d.fault-dec"
                        " aborting\n",hm2->llio->name, i);
                goto fail0;
            }
        
            r = hal_param_u32_newf(HAL_RW, &(inst->fault_lim),
                                   hm2->llio->comp_id, 
                                   "%s.sserial.port-%1d.fault-lim",
                                   hm2->llio->name, i);
            if (r < 0) {
                HM2_ERR("error adding parameter %s.sserial.port-%1d.fault-lim"
                        " aborting\n",hm2->llio->name, i);
                goto fail0;
            }
            //parameter defaults;
            inst->fault_dec = 1;
            inst->fault_inc = 10;
            inst->fault_lim = 200;
            
            //only move to the next instance if this one contains things.
            //hm2->sserial.instance[0] will be the lowest numbered module
            //with attached hardware.
            
            count++ ;
        }
    }
    

    hm2->sserial.num_instances = count; // because of the extra increment

    if (hm2->sserial.num_instances > 0){

        // create the HAL pins
        if (hm2_8i20_create(hm2, md) < 0) goto fail0;
        if (hm2_7i64_create(hm2, md) < 0) goto fail0;
        if (hm2_auto_create(hm2, md) < 0) goto fail0;
        if (hm2_sserial_config_create(hm2) < 0) goto fail0;
        if (hm2_8i20_params(hm2) < 0) goto fail0;
        
        // Set up TRAM writes

        for (i = 0 ; i < hm2->sserial.num_instances ; i++){
            hm2_sserial_instance_t  *inst = &hm2->sserial.instance[i];
            int err_flag;
            if (inst->num_all > 0 ){
                // setup read-back in all modes

                r = hm2_register_tram_read_region(hm2, inst->command_reg_addr,
                                                  sizeof(u32),
                                                  &inst->command_reg_read);
                if (r < 0) {
                    HM2_ERR("error registering tram write region for sserial"
                            "command register (%d)\n", i);
                    goto fail0;
                }

                r = hm2_register_tram_read_region(hm2, inst->data_reg_addr,
                                                  sizeof(u32),
                                                  &inst->data_reg_read);
                if (r < 0) {
                    HM2_ERR("error registering tram write region for sserial "
                            "command register (%d)\n", i);
                    goto fail0;

                }
                // Nothing happens without a "Do It" command
                r = hm2_register_tram_write_region(hm2, inst->command_reg_addr,
                                                   sizeof(u32),
                                                   &inst->command_reg_write);
                if (r < 0) {
                    HM2_ERR("error registering tram write region for sserial "
                            "command register (%d)\n", i);
                    goto fail0;

                }
            }
            // Normal start to check for errors, as soon it will be too late to 
            // abort the load. 
            
            err_flag = 0;
            if (hm2_sserial_stopstart(hm2, md, inst, 0x900) < 0 ) {goto fail0;}
            if (hm2_sserial_auto_check(hm2, inst) < 0) {err_flag = -EINVAL;}
            if (hm2_sserial_8i20_check(hm2, inst) < 0) {/* do nothing */}
            if (hm2_sserial_7i64_check(hm2, inst) < 0) {err_flag = -EINVAL;}
            if (err_flag) goto fail0;
        }
    }
    
    
    // Stop the sserial ports.
    buff=0x800; //Stop All
    for (i = 0 ; i < hm2->sserial.num_instances ; i++) {
        hm2_sserial_instance_t *inst = &hm2->sserial.instance[i];
        hm2->llio->write(hm2->llio, inst->command_reg_addr, &buff, sizeof(u32));
    }
    // Return the physical ports to default
    ddr_reg = 0;
    src_reg = 0;
    for (port  = 0; port < hm2->ioport.num_instances; port ++) {
        hm2->llio->write(hm2->llio, hm2->ioport.ddr_addr + 4 * port,
                         &ddr_reg, sizeof(u32));
        hm2->llio->write(hm2->llio, hm2->ioport.alt_source_addr + 4 * port,
                         &src_reg, sizeof(u32));
    }
    return hm2->sserial.num_instances;

fail0:
    hm2_sserial_cleanup(hm2);
    hm2->sserial.num_instances = 0;
    return -EINVAL;
}


void hm2_sserial_prepare_tram_write(hostmot2_t *hm2, long period){
    // This function mainly serves to handle starting and stopping of the
    // smart-serial modules. Useful for restarting broken comms.
    // Actual tram setup is handled by the sub-drivers

    static int doit_err_count, comm_err_flag; // to avoid repeating error messages
    int i,f;

    if (hm2->sserial.num_instances <= 0) return;

    hm2_8i20_prepare_tram_write(hm2);
    hm2_7i64_prepare_tram_write(hm2);
    hm2_auto_prepare_tram_write(hm2);


    for (i = 0 ; i < hm2->sserial.num_instances ; i++ ) {
        // a state-machine to start and stop the ports, and to
        // supply Do-It commands when required.

        hm2_sserial_instance_t *inst = &hm2->sserial.instance[i];

        switch (*inst->state){
            case 0: // Idle
                if (! *inst->run){ return; }
                *inst->state = 0x11;
                inst->timer = 0;
                //set the modes for the cards
                hm2_8i20_setmode(hm2, inst);
                hm2_7i64_setmode(hm2, inst);
                hm2_sserial_auto_setmode(hm2, inst);

                *inst->command_reg_write = 0x900 | inst->tag_all;
                HM2_DBG("Tag-All = %x\n", inst->tag_all);
                *inst->fault_count = 0;
                doit_err_count = 0;
                comm_err_flag = 0;
                break;
            case 0x01: // normal running
                if (!*inst->run){
                     *inst->state = 0x02;
                    break;
                }
                if (*inst->fault_count > inst->fault_lim) {
                    // If there have been a large percentage of misses, for quite
                    // a long time, it's time to take it seriously. 
                    HM2_ERR("Smart Serial Comms Error: "
                            "There have been more than %i errors in %i "
                            "thread executions at least %i times. "
                            "See other error messages for details.\n",
                            inst->fault_dec, 
                            inst->fault_inc,
                            inst->fault_lim);
                    HM2_ERR("***Smart Serial Port %i will be stopped***\n",i); 
                    *inst->state = 0x20;
                    *inst->command_reg_write = 0x800; // stop command
                    break;
                }
                if (*inst->command_reg_read) {
                    if (doit_err_count < 6){ doit_err_count++; }
                    if (doit_err_count == 4 ){ // ignore 4 errors at startup
                        HM2_ERR("Smart Serial port %i: DoIt not cleared from previous "
                                "servo thread. Servo thread rate probably too fast. "
                                "This message will not be repeated, but the " 
                                "%s.sserial.%1d.fault-count pin will indicate "
                                "if this is happening frequently.\n",
                                i, hm2->llio->name, i);
                    }
                    *inst->fault_count += inst->fault_inc;
                    *inst->command_reg_write = 0x80000000; // set bit31 for ignored cmd
                    break; // give the register chance to clear
                }
                if (*inst->data_reg_read & 0xff){ // indicates a failed transfer
                    *inst->fault_count += inst->fault_inc;
                    f = (*inst->data_reg_read & (comm_err_flag ^ 0xFF));
                    if (f != 0 && f != 0xFF){
                        comm_err_flag |= (f & -f); //mask LSb
                        HM2_ERR("Smart Serial Error: port %i channel %i. " 
                                "You may see this error if the FPGA card "
                                """read"" thread is not running. "
                                "This error message will not repeat.\n",
                                i, ffs(f) - 1);
                        hm2_sserial_auto_check(hm2, inst);
                        hm2_sserial_8i20_check(hm2, inst);
                        hm2_sserial_7i64_check(hm2, inst);
                    }
                }
                        
                if (*inst->fault_count > inst->fault_dec) {
                    *inst->fault_count -= inst->fault_dec;
                }
                else
                {
                    *inst->fault_count = 0;
                }
                
                *inst->command_reg_write = 0x1000 | inst->tag_all;
                break;

            case 0x02: // run to stop transition
                *inst->state = 0x10;
                inst->timer = 0;
                *inst->command_reg_write = 0x800;
                break;
            case 0x10:// wait for normal stop
            case 0x11:// wait for normal start
                inst->timer += period;
                if (*inst->command_reg_read != 0) {
                    if (inst->timer < 2100000000) {
                        break;
                    }
                    HM2_ERR("sserial_write:"
                            "Timeout waiting for CMD to clear\n");
                    *inst->fault_count += inst->fault_inc;
                    // carry on, nothing much we can do about it
                }
                *inst->state &= 0x0F;
                *inst->command_reg_write = 0x80000000; // mask pointless writes
                break;
            case 0x20:// Do-nothing state for serious errors. require run pin to cycle
                *inst->command_reg_write = 0x80000000; // set bit31 for ignored cmd
                if ( ! *inst->run){*inst->state = 0x02;}
                break;
            default: // Should never happen
                HM2_ERR("Unhandled run/stop configuration in \n"
                        "hm2_sserial_write (%x)\n",
                        *inst->state);
                *inst->state = 0;
        }
    }
}

void hm2_sserial_process_tram_read(hostmot2_t *hm2, long period){

    hm2_8i20_process_tram_read(hm2);
    hm2_7i64_process_tram_read(hm2);
    hm2_auto_process_tram_read(hm2);
    hm2_sserial_process_config(hm2, period);

}

int hm2_sserial_config_create(hostmot2_t *hm2){
    int r;
    // create the global config pins

    r = hal_pin_bit_newf(HAL_IN, &(hm2->sserial.hal->read),
                         hm2->llio->comp_id, "%s.sserial.read",
                         hm2->llio->name);
    if (r < 0) { HM2_ERR("error adding pin %s.sserial.read. aborting\n",
                         hm2->llio->name);
        goto fail0;}

    r = hal_pin_bit_newf(HAL_IN, &(hm2->sserial.hal->write),
                         hm2->llio->comp_id, "%s.sserial.write",
                         hm2->llio->name);
    if (r < 0) {HM2_ERR("error adding pin %s.sserial.write. aborting\n",
                        hm2->llio->name);
        goto fail0;}
    r = hal_pin_u32_newf(HAL_IN, &(hm2->sserial.hal->parameter),
                         hm2->llio->comp_id, "%s.sserial.parameter",
                         hm2->llio->name);
    if (r < 0) {
        HM2_ERR("error adding pin %s.sserial.port. aborting\n",
                hm2->llio->name);
        goto fail0;}
    r = hal_pin_u32_newf(HAL_IN, &(hm2->sserial.hal->port),
                         hm2->llio->comp_id, "%s.sserial.port",
                         hm2->llio->name);
    if (r < 0) {
        HM2_ERR("error adding pin %s.sserial.parameter. aborting\n",
                hm2->llio->name);
        goto fail0;}
    r = hal_pin_u32_newf(HAL_IN, &(hm2->sserial.hal->channel),
                         hm2->llio->comp_id, "%s.sserial.channel",
                         hm2->llio->name);
    if (r < 0) {
        HM2_ERR("error adding pin %s.sserial.channel. aborting\n",
                hm2->llio->name);
        goto fail0;}
    r = hal_pin_u32_newf(HAL_IN, &(hm2->sserial.hal->value),
                         hm2->llio->comp_id, "%s.sserial.value",
                         hm2->llio->name);
    if (r < 0) { HM2_ERR("error adding pin %s.sserial.value. aborting\n",
                         hm2->llio->name);
        goto fail0;}
    r = hal_pin_u32_newf(HAL_OUT, &(hm2->sserial.hal->state),
                         hm2->llio->comp_id, "%s.sserial.state",
                         hm2->llio->name);
    if (r < 0) {HM2_ERR("error adding pin %s.sserial.state. aborting\n",
                        hm2->llio->name);
        goto fail0;}

    return 0;

fail0:
    return r;
}

void hm2_sserial_process_config(hostmot2_t *hm2, long period){

    static long timer;
    u32 buff;
    int chan, port, i;

    static hm2_sserial_instance_t *inst;
    static hm2_sserial_tram_t *tram;

    if (hm2->sserial.num_instances <= 0) return;

    chan = *hm2->sserial.hal->channel;
    port = *hm2->sserial.hal->port;

    // What follows is a state machine that handles the handshaking and communications
    // to allow reading and setting of the firmware parameters of arbitrary sserial
    // devices. It responds to tram-read data, so is in this function despite doing
    // some direct llio writes to non-TRAM registers.

    switch (*hm2->sserial.hal->state) {
        case 0: // idle
            if (*hm2->sserial.hal->read == 0 && *hm2->sserial.hal->write == 0) {
                return;
            }
            for (i = 0 ; i < hm2->sserial.num_instances &&
                 hm2->sserial.instance[i].module_index != port ; i++){
            }
            if (i >= hm2->sserial.num_instances) {
                HM2_ERR("sserial port %i not found\n", port);
                *hm2->sserial.hal->read = 0;
                *hm2->sserial.hal->write = 0;
                return;
            }
            inst = &hm2->sserial.instance[i];

            if (*inst->run) {
                HM2_ERR("It is not possible to set or read parameters on "
                        "active/running sserial ports\n");
                *hm2->sserial.hal->read = 0;
                *hm2->sserial.hal->write = 0;
                return;
            }

            if ((inst->tag_all & (1 << chan)) == 0) {
                HM2_ERR("No device to configure on sserial port %i channel %i\n",
                        port, chan);
                *hm2->sserial.hal->read = 0;
                *hm2->sserial.hal->write = 0;
                return;
            }

            tram = NULL ; // Make sure we don't accidentally configure the previous one

            if (inst->tag_8i20 & (1 << chan)) {
                for (i = 0
                     ; (i < inst->num_8i20) && (inst->tram_8i20[i].index != chan)
                     ; i++){
                }
                if (i >= inst->num_8i20) {
                    HM2_ERR("8i20 on sserial port %i, channel %i not found\n",
                            port, chan);
                    *hm2->sserial.hal->read = 0;
                    *hm2->sserial.hal->write = 0;
                    return;
                }
                tram = &inst->tram_8i20[i];
            }
            else if (inst->tag_7i64 & (1 << chan)) {
                for (i = 0
                     ; (i < inst->num_7i64) && (inst->tram_7i64[i].index != chan)
                     ; i++){}
                if (i >= inst->num_7i64) {
                    HM2_ERR("7i64 on sserial port %i, channel %i not found\n",
                            port, chan);
                    *hm2->sserial.hal->read = 0;
                    *hm2->sserial.hal->write = 0;
                    return;
                }
                tram = &inst->tram_7i64[i];
            }
            else if (inst->tag_auto & (1 << chan)) {
                for (i = 0
                     ; (i < inst->num_auto) && (inst->tram_auto[i].index != chan)
                     ; i++){}
                if (i >= inst->num_7i64) {
                    HM2_ERR("auto-config card on sserial port %i, channel %i "
                            "not found\n", port, chan);
                    *hm2->sserial.hal->read = 0;
                    *hm2->sserial.hal->write = 0;
                    return;
                }
                tram = &inst->tram_auto[i];
            }
            /***********************************************************
             // Add other sserial devices here...
             ************************************************************/

            if (tram == NULL) { // didn't find a tram...
                HM2_ERR("Unable to find a supported device for the specified "
                        "channel\n");
                *hm2->sserial.hal->read = 0;
                *hm2->sserial.hal->write = 0;
                return;
            }
            // valid port and channel, so act on the read/write requests
            if (*hm2->sserial.hal->parameter & 0xFF000000) {
                *hm2->sserial.hal->state = 1;}
            else if (*hm2->sserial.hal->write) {*hm2->sserial.hal->state = 9;}
            else if (*hm2->sserial.hal->read) { *hm2->sserial.hal->state = 5; }

            *inst->command_reg_write = 0xF00 | (1 << tram->index); // start in setup mode
            timer = 0;
            break;

        case 1: // eeprom or nvram access flag setup - wait for command clear
            *inst->command_reg_write = 0x80000000; // write mask
            if (*inst->run != 0) {
                *hm2->sserial.hal->state = 20;
                break;}
            timer += period;
            if (*inst->command_reg_read != 0) {
                if (timer > 50000000) {
                    *hm2->sserial.hal->state = 0;
                    *hm2->sserial.hal->read = 0;
                    HM2_ERR("SSerial setup mode start: Timeout in state 1\n");
                }
                break;
            }
            if (*inst->data_reg_read & (1 << tram->index)){
                *hm2->sserial.hal->state = 20;
                HM2_ERR("Channel-not-ready error in parameter write case 1\n"
                        "Data Reg = %08X\nCS Reg = %08X\n",
                        *inst->data_reg_read, *tram->reg_cs_read);
                //break;
            }
            buff = (*hm2->sserial.hal->parameter & 0xFF000000); //flag
            hm2->llio->write(hm2->llio, tram->reg_cs_addr, &buff, sizeof(u32));
            //reg_0 and CMD are always in the TRAM
            //..memory type
            *tram->reg_0_write = (*hm2->sserial.hal->parameter & 0x00FF0000)>>16;
            //..DoIt command
            *inst->command_reg_write = 0x1000 | (1 << tram->index);
            timer = 0;

            if (*hm2->sserial.hal->write) {*hm2->sserial.hal->state = 9;}
            else if (*hm2->sserial.hal->read) { *hm2->sserial.hal->state = 5; }
            else {
                HM2_ERR("Read or Write pins reset part-way through special "
                        "access mode setup, clearing access and aborting\n");
                *hm2->sserial.hal->state = 20;
            }
            break;

        case 5: // read request - wait for command clear
            *inst->command_reg_write = 0x80000000; // write mask
            if (*inst->run != 0) {
                *hm2->sserial.hal->state = 20;
                break;}
            timer += period;
            if (*inst->command_reg_read != 0) {
                if (timer > 50000000) {
                    *hm2->sserial.hal->state = 0;
                    *hm2->sserial.hal->read = 0;
                    HM2_ERR("SSerial setup param read: Timeout in state 5\n");
                }
                break;
            }
            if (*inst->data_reg_read & (1 << tram->index)){
                *hm2->sserial.hal->state = 20;
                HM2_ERR("Channel-not-ready error waiting to send param read"
                        "command (%x)\n", *inst->data_reg_read);
                break;
            }
            // read command setup
            buff = 0x45000000 | (*hm2->sserial.hal->parameter & 0x0000FFFF);
            hm2->llio->write(hm2->llio, tram->reg_cs_addr, &buff, sizeof(u32));
            // Do It command (written by tram)
            *inst->command_reg_write =  0x1000 | (1 << tram->index);
            timer = 0;
            *hm2->sserial.hal->state = 6;
            break;
        case 6: // waiting for read request ack.
            *inst->command_reg_write = 0x80000000; // write mask bit
            if (*inst->run != 0) { *hm2->sserial.hal->state = 0; return;}
            timer += period;
            if (*inst->command_reg_read != 0) {
                if (timer > 750000000) {
                    *hm2->sserial.hal->state = 20;
                    HM2_ERR("Channel-not-ready error in parameter write case 6\n"
                            "Data Reg = %08X\nCS Reg = %08X\n",
                            *inst->data_reg_read, *tram->reg_cs_read);
                    break;
                }
                break;
            }
            *hm2->sserial.hal->state = 20;
            if (*inst->data_reg_read & (1 << tram->index)){
                HM2_ERR("Channel-not-ready error in state 6: %x, %x\n"
                        "errror flags %d\n",
                        *inst->data_reg_read, (1 << tram->index),
                        (*tram->reg_cs_read & 0x0000FF00) >> 8);
                //              break;
            }
            // looks like success
            *hm2->sserial.hal->value = *tram->reg_0_read;
            break;

        case 9: // write command setup
            *inst->command_reg_write = 0x80000000; // write mask
            if (*inst->run != 0) {*hm2->sserial.hal->state = 0 ; return; }
            timer += period;
            if (*inst->command_reg_read != 0) {
                if (timer > 750000000) {
                    *hm2->sserial.hal->state = 20;
                    HM2_ERR("SSerial setup param write: Timeout in state 9\n");
                    break;
                }
            }
            if (*inst->data_reg_read & (1 << tram->index)){
                *hm2->sserial.hal->state = 20;
                HM2_ERR("Channel-not-ready error in parameter write case 9\n"
                        "Data Reg = %08X\nCS Reg = %08X\n",
                        *inst->data_reg_read, *tram->reg_cs_read);
                break;
            }
            // write command setup
            *tram->reg_0_write = *hm2->sserial.hal->value;
            buff = 0x65000000 | (*hm2->sserial.hal->parameter & 0x0000FFFF);
            hm2->llio->write(hm2->llio, tram->reg_cs_addr, &buff, sizeof(u32));
            // Do It command
            *inst->command_reg_write = 0x1000 | (1 << tram->index);
            timer = 0;
            *hm2->sserial.hal->state = 10;
            break;
        case 10: // waiting for write request ack.
            *inst->command_reg_write = 0x80000000; // write mask
            if (*inst->run != 0) {*hm2->sserial.hal->state = 0; return;}
            timer += period;
            if (*inst->command_reg_read != 0) {
                if (timer > 50000000) {
                    *hm2->sserial.hal->state = 20;
                    HM2_ERR("SSerial setup param write: Timeout in state 10\n");
                    break;
                }
                break;
            }
            *hm2->sserial.hal->state = 20;
            if (*inst->data_reg_read & (1 << tram->index)){
                HM2_ERR("Channel-not-ready error in parameter write case 10\n"
                        "Data Reg = %08X\nCS Reg = %08X\n",
                        *inst->data_reg_read, *tram->reg_cs_read);
            }
            break;
        case 20: // Clear flags and read/write pins
            *hm2->sserial.hal->write = 0;
            *hm2->sserial.hal->read = 0;

            buff = 0x0; // Clear MSB of CSR
            hm2->llio->write(hm2->llio, tram->reg_cs_addr, &buff, sizeof(u32));

            if (!(*hm2->sserial.hal->parameter & 0xFF000000)) {
                *inst->command_reg_write = 0x800;
                *hm2->sserial.hal->state = 22;
                break;
            }
            buff = (*hm2->sserial.hal->parameter & 0xFF000000); //flag
            hm2->llio->write(hm2->llio, tram->reg_cs_addr, &buff, sizeof(u32));
            *tram->reg_0_write = 0; //clear type flag
            *inst->command_reg_write = 0x1000 |  (1 << tram->index); // DoIt command
            timer = 0;
            *hm2->sserial.hal->state = 21;
            break;
        case 21:
            *inst->command_reg_write = 0x80000000; // write mask
            timer += period;
            if (*inst->command_reg_read != 0) {
                if (timer > 750000000) {
                    *inst->command_reg_write = 0x800; // stop request
                    *hm2->sserial.hal->state = 22;
                    HM2_ERR("Timeout in state 21, attempting to reset flags."
                            " suggest hard reset of cards (power off) \n");
                    break;
                }
                break;
            }
            if (*inst->data_reg_read & (1 << tram->index)){
                HM2_ERR("Channel-not-ready error in case 21, clearing flags\n"
                        "Data Reg = %08X\nCS Reg = %08X\n",
                        *inst->data_reg_read, *tram->reg_cs_read);
            }
            *inst->command_reg_write = 0x800; // stop request
            *hm2->sserial.hal->state = 22;
            break;
        case 22: // wait for port to stop
            *inst->command_reg_write = 0x80000000; // write mask
            timer += period;
            if (*inst->command_reg_read != 0) {
                if (timer > 220000000) {
                    *hm2->sserial.hal->state = 0;
                    HM2_ERR("Timeout waiting for port to stop \n");
                    break;
                }
                break;
            }
            *hm2->sserial.hal->state = 0;
            break;

        default:
            HM2_ERR("Unsupported state (%i) in sserial setup\n", *hm2->sserial.hal->state);
            *hm2->sserial.hal->state = 0;
            break;
    }
    if (*hm2->sserial.hal->state != 0){
        HM2_DBG("COM(r/w):%08x/%08x, DATA:%08x, CS:%08x, USER0:%08x\n", 
                *inst->command_reg_read, *inst->command_reg_write, 
                *inst->data_reg_read, buff, *tram->reg_0_write);
    }

}

void hm2_sserial_print_module(hostmot2_t *hm2) {
    int i;
    HM2_PRINT("SSerial: %d\n", hm2->sserial.num_instances);
    HM2_PRINT("  version %d\n", hm2->sserial.version);
    if (hm2->sserial.num_instances <= 0) return;
    for (i = 0; i < hm2->sserial.num_instances; i ++) {
        HM2_PRINT("    instance %d:\n", i);
        HM2_PRINT("        Command Addr 0x%04x\n", hm2->sserial.instance[i].command_reg_addr);
        HM2_PRINT("        Data Addr    0x%04x\n", hm2->sserial.instance[i].data_reg_addr);
        HM2_PRINT("        %d 8i20s\n", hm2->sserial.instance[i].num_8i20);
        HM2_PRINT("        %d 7i64s\n", hm2->sserial.instance[i].num_7i64);
    }
    HM2_PRINT("\n");
}

u32 hm2_sserial_get_param(hostmot2_t *hm2, hm2_sserial_tram_t *chan, int param){
    u32 data;

    if (0 > hm2_sserial_waitfor(hm2, chan->reg_cs_addr, 0x0000FF00, 20)){
        goto fail0;
    }

    data = 0x45000000 | param;
    hm2->llio->write(hm2->llio, chan->reg_cs_addr, &data, sizeof(u32));
    data = 0x1000 | (1 << chan->index);
    hm2->llio->write(hm2->llio, chan->reg_command_addr, &data, sizeof(u32));

    if (0 > hm2_sserial_waitfor(hm2, chan->reg_command_addr, 0xFFFFFFFF, 20)){
        goto fail0;
    }
    if (0 > hm2_sserial_waitfor(hm2, chan->reg_cs_addr, 0x00000FF00, 20)){
        goto fail0;
    }

    hm2->llio->read(hm2->llio, chan->reg_data_addr, &data, sizeof(u32));
    hm2->llio->read(hm2->llio, chan->reg_0_addr, &data, sizeof(u32));
    return data;
fail0:
    return -EINVAL;
}

int hm2_sserial_get_bytes(hostmot2_t *hm2, hm2_sserial_tram_t *chan, void *buffer, int addr, int size ){
    // Gets the bytes one at a time. This could be done more efficiently. 
    char *ptr;
    if (0 > hm2_sserial_waitfor(hm2, chan->reg_cs_addr, 0x0000FF00, 20)){
        goto fail0;
    }
    ptr = (char*)buffer;
    while(0 != size){ // -1 in size means "find null" for strings.
        
        u32 data = 0x4C000000 | addr++;
        hm2->llio->write(hm2->llio, chan->reg_cs_addr, &data, sizeof(u32));
        
        if (0 > hm2_sserial_waitfor(hm2, chan->reg_cs_addr, 0x0000FF00, 20)){
            goto fail0;
        }
        data = 0x1000 | (1 << chan->index);
        hm2->llio->write(hm2->llio, chan->reg_command_addr, &data, sizeof(u32));
        
        if (0 > hm2_sserial_waitfor(hm2, chan->reg_command_addr, 0xFFFFFFFF, 20)){
            goto fail0;
        }
        
        hm2->llio->read(hm2->llio, chan->reg_0_addr, &data, sizeof(u32));
        data &= 0x000000FF;
        size--;
        if (size < 0) { // string data
            if (data == 0 || size < (-HM2_SSERIAL_MAX_STRING_LENGTH)){
                size = 0; 
            } else {
                data |= 0x20; // lower case
            }
        }
        *(ptr++) = (unsigned char)data;
    }
    return addr;
        
fail0:
    HM2_ERR("Error trying to read config data in sserial_get_bytes\n");
    return -EINVAL;
}

int hm2_sserial_stopstart(hostmot2_t *hm2, hm2_module_descriptor_t *md, 
                          hm2_sserial_instance_t *inst, u32 start_mode){
    u32 buff, addr;
    int i = inst->module_index;
    int c;
    
    buff=0x800; //Stop All
    hm2->llio->write(hm2->llio, inst->command_reg_addr, &buff, sizeof(u32));
    if (hm2_sserial_waitfor(hm2, inst->command_reg_addr, 0xFFFFFFFF,51) < 0){
        return -EINVAL;
    }
    
    for (c = 0 ; c < inst->num_channels ; c++){
        if (hm2->config.sserial_modes[i][c] != 'x'){
            start_mode |= 1 << c;
            HM2_DBG("Start-mode = %x\n", start_mode);
            // CS addr - write card mode
            addr = md->base_address + 2 * md->register_stride
            + i * md->instance_stride + c * sizeof(u32);
            buff = (hm2->config.sserial_modes[i][c] - '0') << 24;
            hm2->llio->write(hm2->llio, addr, &buff, sizeof(u32));
        }
    }
    hm2->llio->write(hm2->llio, inst->command_reg_addr, &start_mode, sizeof(u32));
    if (hm2_sserial_waitfor(hm2, inst->command_reg_addr, 0xFFFFFFFF, 8000) < 0){
        return -EINVAL;
    }
    return 0;
}

int hm2_sserial_check_errors(hostmot2_t *hm2, hm2_sserial_tram_t *tram){
    u32 buff;
    int i;
    int err_flag = 0;
    u32 err_mask = 0xF300E1FF;
    const char *err_list[32] = {"CRC error", "Invalid cookie", "Overrun",
        "Timeout", "Extra character", "Serial Break Error", "Remote Fault", 
        "Too many errors", 
        
        "Remote fault", "unused", "unused", "unused", "unused", 
        "Communication error", "No Remote ID", "Communication Not Ready",
        
        "unused","unused","unused","unused","unused","unused","unused","unused",
        
        "Watchdog Fault", "No Enable", "Over Temperature", "Over Current", 
        "Over Voltage", "Under Voltage", "Illegal Remote Mode", "LBPCOM Fault"};
    
    hm2->llio->read(hm2->llio, tram->reg_data_addr, &buff, sizeof(u32));
    if (0 == (buff & (1 << tram->index))) {return 0;}
    
    hm2->llio->read(hm2->llio, tram->reg_cs_addr, &buff, sizeof(u32));
    buff &= err_mask;
    for (i = 31 ; i > 0 ; i--){
        if (buff & (1 << i)) {
            HM2_ERR("Smart serial card %s error = (%i) %s\n", 
                    tram->name, i, err_list[i]);
            err_flag = -EINVAL;
        }
    }
    return err_flag;
}

int hm2_sserial_waitfor(hostmot2_t *hm2, u32 addr, u32 mask, int ms){
    u64 t1, t2;
    u32 d;
    t1 = rtapi_get_time();
    do { // wait for addr to clear
        hm2->llio->read(hm2->llio, addr, &d, sizeof(u32));
        t2 = rtapi_get_time();
        if ((u32)(t2 - t1) > 1000000L * ms) {
            HM2_ERR("hm2_sserial_waitfor: Timeout (%dmS) waiting for addr %x &"
                    "mask %x val %x\n", ms, addr, mask, d & mask);
            addr += 0x100;
            hm2->llio->read(hm2->llio, addr, &d, sizeof(u32));
            HM2_ERR("DATA addr %x after timeout: %x\n", addr, d);
            return -1;
        }
    }while (d & mask);
    return 0;
}

void hm2_sserial_force_write(hostmot2_t *hm2){
    int i;
    u32 buff;
    for(i = 0; i < hm2->sserial.num_instances; i++){

        buff = 0x800;
        hm2->llio->write(hm2->llio, hm2->sserial.instance[i].command_reg_addr, &buff, sizeof(u32));
        *hm2->sserial.instance[i].run = 0;
        *hm2->sserial.instance[i].state = 0;
        hm2_sserial_waitfor(hm2, hm2->sserial.instance[i].command_reg_addr, 0xFFFFFFFF, 20);
        *hm2->sserial.instance[i].run = 1;
        *hm2->sserial.instance[i].command_reg_write = 0x80000000;
    }
}

void hm2_sserial_cleanup(hostmot2_t *hm2){
    int i;
    u32 buff;
    for (i = 1 ; i < hm2->sserial.num_instances; i++){
        //Shut down the sserial devices rather than leave that to the watchdog. 
        buff = 0x800;
        hm2->llio->write(hm2->llio,
                         hm2->sserial.instance[i].command_reg_addr,
                         &buff,
                         sizeof(u32));
        if (hm2->sserial.instance[i].tram_8i20 != NULL){
            hm2_sserial_8i20_cleanup(hm2);
        }
        if (hm2->sserial.instance[i].tram_7i64 != NULL){
            hm2_sserial_7i64_cleanup(hm2);
        }
        if (hm2->sserial.instance[i].tram_auto != NULL){
            hm2_sserial_auto_cleanup(hm2);
        }
    }
}

