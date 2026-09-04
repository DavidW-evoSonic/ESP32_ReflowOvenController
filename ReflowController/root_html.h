const char ROOT_HTML[] PROGMEM = R"=====(
<html>
  <head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
      /* This is a fixed-viewport app: the page itself must never scroll.
         Without this the chart's SVG overflows its box by a pixel or two, the
         page gains a scrollbar, that narrows the viewport, the chart redraws
         smaller, the scrollbar goes away -- and it oscillates on alternate
         refreshes. Panels that can genuinely exceed the viewport scroll
         themselves instead; see #controls. */
      html, body { height: 100%; overflow: hidden; }
      body { margin: 0; display: flex; flex-direction: column;
             font-family: system-ui, -apple-system, "Segoe UI", Roboto, sans-serif; }

      /* Live readout. Always visible, above the graph, legible across a
         workshop -- this is the thing you look at while the oven runs. */
      /* Grid, not flex, and every column minmax(0,1fr).
         As flex items the tiles took min-width:auto, so the longest sub-line
         of the second -- "corridor 120.1-132.9 C" -- forced its own tile wider
         and squeezed the others. The row then changed height and the chart
         below it resized, once a second, forever. Equal 0-basis grid tracks
         cannot be widened by their contents. */
      #readout { flex: none; display: grid; gap: 1px;
                 grid-template-columns: repeat(auto-fit, minmax(150px, 1fr));
                 background: #d4d4d8; border-bottom: 1px solid #d4d4d8; }
      #readout .tile { min-width: 0; background: #fafafa; padding: 8px 14px; }
      /* Clip rather than wrap: a wrapped line is the other way the row
         changes height mid-refresh. */
      #readout .label, #readout .value, #readout .sub {
                 white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
      #readout .label { font-size: 11px; font-weight: 600; letter-spacing: .08em;
                        text-transform: uppercase; color: #71717a; }
      #readout .value { font-size: 44px; line-height: 1.05; font-weight: 700;
                        color: #18181b; font-variant-numeric: tabular-nums; }
      #readout .value .unit { font-size: 20px; font-weight: 600; color: #71717a;
                              margin-left: 2px; }
      #readout .sub { font-size: 12px; color: #71717a;
                      font-variant-numeric: tabular-nums; }
      #r_target .value { color: #2563eb; }

      /* The heater pill reports the contact, not the demand -- see #heating. */
      #r_state .value, #r_heat .value { font-size: 34px; letter-spacing: .04em; }
      /* Reserve the tall line height on every tile regardless of which font
         size it uses, so swapping "Ready" for "EXTENDING" cannot move the
         row. */
      #readout .value { min-height: 46px; }
      #r_heat.on  { background: #fef2f2; }
      #r_heat.on  .value { color: #dc2626; }
      #r_heat.off .value { color: #a1a1aa; }

      /* Controller die temperature. Amber and red rather than a number that
         has to be interpreted -- the point of this tile is to be noticed from
         across the room, not read. */
      #r_mcu.warn { background: #fffbeb; }
      #r_mcu.warn .value { color: #b45309; }
      #r_mcu.hot  { background: #fef2f2; }
      #r_mcu.hot  .value { color: #dc2626; }

      /* An extension is a departure from the profile the step declared, so it
         gets its own colour rather than hiding in the state name. */
      #r_state.ext { background: #fffbeb; }
      #r_state.ext .value { color: #b45309; }

      /* No fresh /status: the numbers on screen are history, so say so. */
      #readout.stale { opacity: .45; }
      #readout.stale .value::after { content: " ?"; color: #a1a1aa; }

      #stage { position: relative; flex: 1 1 auto; min-height: 0;
               overflow: hidden; }
      /* Absolutely positioned, not width/height 100%.
         A percentage height on a Google Charts container inside a flex child
         is a sizing feedback loop: the chart measures the box, draws an SVG,
         the SVG participates in the flex child's height, and the next draw
         measures something different. inset:0 gives it a definite size that
         its own content cannot influence. */
      #chart_div { position: absolute; inset: 0; overflow: hidden; }
      /* The settings panel is taller than the viewport on a laptop, and the
         page can no longer scroll to reach it -- so it scrolls itself. */
      #controls { position: absolute; top: 0; left: 0; z-index: 1000;
                  max-height: 100%; overflow-y: auto; overscroll-behavior: contain;
                  background: rgba(255,255,255,0.92); padding: 6px; }
    </style>
    <script type="text/javascript" src="https://www.gstatic.com/charts/loader.js"></script>
    <script src="https://ajax.googleapis.com/ajax/libs/jquery/3.4.1/jquery.min.js"></script>
    <script>
      google.charts.load('current', {
        'packages': ['line', 'corechart']
      });
      google.charts.setOnLoadCallback(function () {
        var chartdata;
        function initchartdata(){
            chartdata = new google.visualization.DataTable()
            chartdata.addColumn('number', 'Seconds');
            chartdata.addColumn({type: 'string', role: 'annotation'});
            chartdata.addColumn('number', "Temperature");
            chartdata.addColumn('number', "Setpoint");
            chartdata.addColumn('number', "Corridor low");
            chartdata.addColumn('number', "Corridor high");
            chartdata.addColumn('number', "Power");
            coastRows = 0;
        }
        initchartdata();

        var classicOptions = {
          title: 'ReflowOven',
          // Gives each series an axis that matches the vAxes number below.
          series: {
            0: {
              targetAxisIndex: 0
            },
            1: {
              targetAxisIndex: 0
            },
            2: {
              targetAxisIndex: 0,
              lineDashStyle: [3, 3]
            },
            3: {
              targetAxisIndex: 0,
              lineDashStyle: [3, 3]
            },
            4: {
              targetAxisIndex: 1
            }
          },
          vAxes: {
            // Adds titles to each axis.
            0: {
              title: 'Temperature (°C)',
              viewWindow: {
                max:300,
                min:0
              }
            },
            1: {
              title: 'Power (%)',
              viewWindow: {
                max:100,
                min:0
              }
            }
          },
          hAxis: {
              title: 'Time (s)',
              viewWindow: {
                  max: 100
              },
          },
          // Pin the plot rectangle. Left to itself Google Charts sizes it from
          // the axis label widths, and hAxis.viewWindow.max grows every second
          // during a run (100, 110, 120...), so the tick labels change width
          // and the whole plot shifts on each redraw.
          chartArea: {
            left: '9%', right: '9%', top: '11%', bottom: '15%'
          },
            annotations: {
               alwaysOutside: true,
                 style: 'line',
                 highContrast: true,
                 textStyle: {
                      bold: true
                    }
            }
        };

//        var dtime=0;
        var classicChart = new google.visualization.LineChart(document.getElementById('chart_div'));
        var lastState= "";
        var running=false;
        var startMax=null;   // from /config: chamber must be below this to start
        // Complete is not cold. The profile stops where the controller stops
        // driving, and a fault stops it outright with the whole ramp down
        // still ahead -- either way the cooling curve is the part that says
        // whether the chamber sheds heat fast enough to set the joints, and it
        // used to be thrown away the instant the state changed. Keep sampling
        // past Complete, bounded so an oven left sitting warm does not grow
        // the table forever.
        var coastRows=0;
        var COAST_STOP_C=50;    // below this the curve has nothing left to say
        var COAST_MAX_ROWS=900; // 15 min at 1 Hz
        function loadstatus()
        {
          var lastcall=Date.now();
          $.getJSON( "status?t="+Date.now() )
           .done(function(data) {
/*             .always(function() {
              data= {};
              if(dtime<=1000)
                  data.state="Ready";
              else if(dtime<10000)
                  data.state="a";
              else if(dtime<20000)
                  data.state="b";
              else if(dtime<30000)
                  data.state="Complete";
              else 
                  dtime=0;
              data.temp=10*dtime/1000
              data.setpoint=20
              data.power=50
              data.time = dtime;
              dtime=dtime+1000;
*/              

             console.log( "success", data );

             // ---- live readout -------------------------------------
             // Driven off every poll, in every state, so it is never
             // showing a number the controller has moved on from.
             $('#readout').removeClass('stale');
             $('#v_state').text(data.extending ? "EXTENDING" : data.state);
             $('#r_state').toggleClass('ext', !!data.extending);
             // Once any extension time is on the clock it stays on the
             // sub-line for the rest of the run, including through Complete:
             // a stretched profile must not report as an ordinary one.
             var stepTxt = data.steps > 0 && data.state!="Ready"
                 && data.state!="Complete"
                 ? "step "+(data.step+1)+" of "+data.steps
                 : "";
             if (data.extended > 0)
                 stepTxt += (stepTxt ? " \u00B7 " : "")
                          + "+"+Math.round(data.extended)+"s extended";
             $('#v_step').html(stepTxt || "&nbsp;");
             $('#v_temp').text(data.temp.toFixed(1));
             $('#v_rate').text((data.dt>=0?"+":"\u2212")
                 + Math.abs(data.dt).toFixed(2) + " \u00B0C/s");

             // Idle, the firmware parks setpoint and corridor on the
             // current temperature (see the Ready case in the control
             // loop). Showing that as a "target" would invent an
             // intention the oven does not have, so blank it instead.
             var idle = (data.state=="Ready" || data.state=="Complete");
             $('#v_target').html(idle ? "&mdash;" : data.setpoint.toFixed(1));
             $('#v_corridor').html(idle ? "&nbsp;"
                 : "corridor "+data.low.toFixed(1)+"\u2013"
                   +data.high.toFixed(1)+" \u00B0C");

             // data.heating is the relay contact, not the demand. Over a
             // 4 s time-proportional window sampled once a second this
             // will blink at part power -- that is the element being
             // truthfully reported, so the duty sits underneath it to
             // give the blinking its context.
             var on = !!data.heating;
             $('#v_heat').text(on ? "ON" : "OFF");
             $('#r_heat').toggleClass('on', on).toggleClass('off', !on);
             $('#v_duty').text(Math.round(data.power) + "% duty");

             // Controller die temperature -- a hazard readout, not a
             // diagnostic: the electronics sit in an enclosure inside the
             // oven, a sheet of metal from the element. The thresholds live
             // here rather than in the firmware because the firmware takes no
             // action on them. 85 degC is the red line because that is the
             // WROOM-32 module's rated maximum, and the W25Q32 flash's, not
             // because of anything about the die itself.
             var mcu = (typeof data.mcu == "number") ? data.mcu : null;
             if(mcu === null){
                 $('#v_mcu').html("&mdash;");
                 $('#v_mcu_note').html("&nbsp;");
                 $('#r_mcu').removeClass('warn hot');
             } else {
                 // temprature_sens_read() hands back a uint8_t in Fahrenheit,
                 // so the scale stops dead at 255 F = 123.9 degC. Show a
                 // pegged sensor as pegged; it is not a measurement.
                 $('#v_mcu').text(mcu >= 123 ? "\u2265124" : mcu.toFixed(1));
                 $('#r_mcu').toggleClass('hot',  mcu >= 85)
                            .toggleClass('warn', mcu >= 70 && mcu < 85);
                 $('#v_mcu_note').text(
                     mcu >= 85 ? "OVER RATED MAX 85 \u00B0C"
                   : mcu >= 70 ? "hot \u2014 rated max 85 \u00B0C"
                   :             "die temp \u00B7 rated max 85 \u00B0C");
             }
             // -------------------------------------------------------

             if(data.openDoor){
                 $('#door').show();
                 alarmOn();
             } else {
                 $('#door').hide();
                 alarmOff();
                 $('#door_mute').text("Silence").prop('disabled', false);
             }

             if(data.fault){
                 // No "power cycle the oven" here. It was appended to every
                 // fault regardless of cause and told the operator nothing
                 // about the one they had -- and a reboot does not fix any of
                 // the conditions that raise one.
                 $('#fault').text("FAULT: "+data.fault).show();
             }
             // Amber, and deliberately NOT the red fault banner: the run is
             // still going. A warning says the oven fell behind, not that it
             // stopped, and the two must not look alike at a glance.
             if(data.warning){
                 $('#warn').text("Warning: "+data.warning
                                 +" \u2014 the run is continuing").show();
             } else {
                 $('#warn').hide();
             }

             // draw() rebuilds the whole SVG, so it is now called only when
             // the picture has actually changed. It used to run on every poll
             // -- once a second, even sitting idle in Complete with static
             // data -- and each rebuild was a visible relayout.
             var stateChanged = (data.state != lastState);
             var lable   = null;
             var needDraw = false;

             if(data.state=="Ready")
             {
                 // Returning to Ready does NOT clear the chart. It used to,
                 // and that threw away the trace at the worst moment: the
                 // firmware keeps no run history, so between Complete and the
                 // Download Graph button this tab holds the only copy. On
                 // 2026-09-04 a faulted run went Complete -> Ready on a /stop
                 // and took its own evidence with it.
                 //
                 // Cleared when the next run STARTS instead, below, which is
                 // the moment the old trace actually stops being wanted.
                 if(stateChanged) needDraw=true;
                 running=false;
                 $('#action').text(
                     (startMax!==null && data.temp > startMax)
                     ? "Too hot ("+Math.round(data.temp)+"\u00B0C)"
                     : "Start Reflow");
             }
             else{
                 $('#action').text(data.state=="Complete" ? "Reset" : "Cancel");
                 running=true;
             }

             if(stateChanged)
             {
                // A new cycle beginning is what retires the previous trace.
                // Anything that produces a curve counts; arriving at Ready or
                // Complete does not, those being where a trace is read.
                if(data.state!="Ready" && data.state!="Complete"
                   && (lastState=="Ready" || lastState=="Complete")){
                    initchartdata();          // also resets coastRows
                    classicOptions.hAxis.viewWindow.max=100;
                }
                lastState = data.state;
                lable     = data.state;
                needDraw  = true;
                if(data.state=="Complete")
                    classicOptions.hAxis.viewWindow.max=null;
             }

             var coasting = (data.state=="Complete"
                             && coastRows < COAST_MAX_ROWS
                             && data.temp > COAST_STOP_C);
             if(data.state!="Ready" && (data.state!="Complete" || coasting)){
                 // Idle, the firmware parks setpoint and corridor on the
                 // present temperature, so charting them past Complete would
                 // draw a target line that merely traces the thermometer.
                 // Null instead: the corridor series end where the run did and
                 // the temperature carries on alone.
                 if(coasting){
                     coastRows++;
                     chartdata.addRow([data.time/1000, lable, data.temp,
                                       null, null, null, data.power]);
                 } else {
                     chartdata.addRow([data.time/1000, lable, data.temp, data.setpoint,
                                       data.low, data.high, data.power]);
                 }
                 classicOptions.hAxis.viewWindow.max=Math.max(100,Math.round(data.time/1000.0)+10);
                 needDraw=true;
             }

             if(needDraw) classicChart.draw(chartdata, classicOptions);
           })
           .fail(function() {
             $('#readout').addClass('stale');
           })
           .always(function() {
             setTimeout(loadstatus, Math.max(10,1000-(Date.now()-lastcall)));
           });

        }

        // The chart no longer redraws on a timer, so the one thing that still
        // legitimately needs one has to ask for it.
        var resizeTimer=null;
        $(window).on('resize', function(){
            clearTimeout(resizeTimer);
            resizeTimer=setTimeout(function(){
                classicChart.draw(chartdata, classicOptions);
            }, 150);
        });

        loadstatus();

        // Everything below replaces the rotary-encoder menu: profile slots,
        // the step list and manual heating. There are no gains to edit any
        // more -- the corridor law has nothing to tune.

        function act(path, params){
            return $.ajax({
                url: "http://"+window.location.hostname+":8080/"+path,
                data: $.extend({t: Date.now()}, params || {}),
                dataType: "text"
            }).fail(function(x){
                var why = (x.responseText||"").trim();
                alert(x.status==409
                      ? (why && why!="ERROR" ? why
                         : "Controller busy -- stop the cycle first.")
                      : "Communication error!");
            });
        }

        // One step per line, "target, lo, hi" plus an optional unit: bare or
        // "s" for seconds, "r" for degC/s. Both shapes are how paste
        // datasheets state a profile, and which one a step uses decides what
        // stays fixed when the chamber starts warm.
        function loadConfig(){
            $.getJSON("config?t="+Date.now()).done(function(c){
                $('#p_name').val(c.profile.name);
                $('#p_steps').val(c.profile.steps.map(function(s){
                    return s.targetTemp+", "+s.lo+", "+s.hi
                         + (s.bound=="rate" ? ", r" : "");
                }).join("\n"));
                $('#p_limit').text(c.maxSteps);
                $('#o_thermalLag').val(c.oven.thermalLag);
                $('#o_measureTemp').val(c.oven.measureTemp);
                $('#o_calC2').val(c.oven.calC2);
                $('#o_calC1').val(c.oven.calC1);
                $('#o_calC0').val(c.oven.calC0);
                startMax = c.oven.startMaxTemp;
                // Build identity, next to the upload control on purpose:
                // this is what you compare against before an OTA push, and
                // what confirms the push took after the oven reboots.
                //
                // The build stamp is the load-bearing half. The version is
                // hand-maintained and so can lie -- two builds ship as 2.0.1
                // if nobody bumps it -- whereas __DATE__/__TIME__ move on
                // every rebuild whether or not anyone remembered.
                $('#fw_ver').text(c.version
                    ? "Running " + c.version + " \u2014 built " + c.build
                    : "Running: unknown (pre-2.0.1 build)");
                // Never the password itself, only whether one is set.
                $('#ota_none').toggle(!c.otaSet);
                $('#ota_have').toggle(!!c.otaSet);
                $('#o_measured').text(c.oven.measuredLag > 0
                    ? "last measured: "+c.oven.measuredLag+" s"
                    : "not measured this power-up");
            });
            $.getJSON("profiles?t="+Date.now()).done(function(d){
                var sel = $('#slot').empty();
                d.profiles.forEach(function(p){
                    sel.append($('<option>').val(p.id).text(p.id+": "+p.name));
                });
                sel.val(d.active);
            });
        }
        loadConfig();

        $('#p_apply').click(function(){
            var steps = $('#p_steps').val().split("\n").map(function(l){
                return l.trim().replace(/\s+/g, "");
            }).filter(function(l){ return l.length; }).join(";");
            act("profile/edit", {name: $('#p_name').val(), steps: steps})
                .done(loadConfig);
        });
        $('#load').click(function(){
            act("profile/load", {id: $('#slot').val()}).done(loadConfig);
        });
        $('#save').click(function(){
            act("profile/save", {id: $('#slot').val()}).done(loadConfig);
        });
        $('#o_apply').click(function(){
            act("oven", {thermalLag: $('#o_thermalLag').val(),
                         measureTemp: $('#o_measureTemp').val()}).done(loadConfig);
        });
        $('#o_measure').click(function(){
            if(confirm("Measure thermal lag? The oven heats to the measure "
                     + "temperature and is then deliberately allowed to "
                     + "overshoot. It writes the result to the lag setting."))
                act("measurelag");
        });
        $('#manual_set').click(function(){
            act("manual", {power: $('#manual_power').val()});
        });
        $('#cal_apply').click(function(){
            act("oven", {calC2: $('#o_calC2').val(),
                         calC1: $('#o_calC1').val(),
                         calC0: $('#o_calC0').val()}).done(loadConfig);
        });
        $('#hold_set').click(function(){
            act("hold", {temp: $('#hold_temp').val()});
        });
        $('#hold_stop').click(function(){
            act("stop");
        });
        $('#reset').click(function(){
            if(confirm("Factory reset? This erases all profiles and the WiFi credentials."))
                act("factoryreset").done(loadConfig);
        });
        // The oven's buzzer is optional hardware and the door prompt is the
        // one time-critical thing this page says, so the browser carries it
        // too: a tone the laptop makes itself, and the tab title, which is the
        // only channel that still works when the tab is not on screen.
        //
        // Deliberately more insistent than the firmware's three beeps -- it
        // repeats until the prompt clears or you silence it, because a missed
        // prompt at peak is a cooked board.
        var audioCtx  = null;
        var alarmTimer = null;
        var alarmMuted = false;
        var baseTitle  = document.title;

        // Browsers refuse to start audio without a user gesture. Starting a
        // run from this page is one, but a reload mid-cycle is not -- so take
        // any click as permission and resume a suspended context.
        function unlockAudio(){
            try{
                if(!audioCtx)
                    audioCtx = new (window.AudioContext || window.webkitAudioContext)();
                if(audioCtx.state == "suspended") audioCtx.resume();
            }catch(e){ audioCtx = null; }
        }
        $(document).on('click', unlockAudio);

        function beep(freq, ms){
            if(!audioCtx || audioCtx.state != "running") return;
            var osc  = audioCtx.createOscillator();
            var gain = audioCtx.createGain();
            osc.type = "square";
            osc.frequency.value = freq;
            // Ramped, not switched: a hard gate on a square wave clicks.
            gain.gain.setValueAtTime(0.0001, audioCtx.currentTime);
            gain.gain.exponentialRampToValueAtTime(0.25, audioCtx.currentTime + 0.01);
            gain.gain.exponentialRampToValueAtTime(0.0001, audioCtx.currentTime + ms/1000);
            osc.connect(gain); gain.connect(audioCtx.destination);
            osc.start();
            osc.stop(audioCtx.currentTime + ms/1000 + 0.02);
        }

        function alarmOn(){
            if(alarmTimer) return;
            unlockAudio();
            document.title = "\u26A0 OPEN THE DOOR";
            var flip = false;
            alarmTimer = setInterval(function(){
                flip = !flip;
                // Two tones, because a warble carries across a room better
                // than a single pitch does.
                if(!alarmMuted) beep(flip ? 880 : 1320, 220);
                document.title = flip ? "\u26A0 OPEN THE DOOR" : baseTitle;
            }, 700);
            if(!alarmMuted) beep(880, 220);
        }

        function alarmOff(){
            if(alarmTimer){ clearInterval(alarmTimer); alarmTimer = null; }
            alarmMuted = false;
            document.title = baseTitle;
        }

        $('#door_mute').click(function(){
            alarmMuted = true;
            $(this).text("silenced").prop('disabled', true);
        });

        // HTTP basic auth, assembled here rather than letting the browser
        // prompt: the password lives in a field on this panel, and a native
        // 401 dialog mid-upload is a worse experience than a refusal.
        function otaAuth(pass){ return "Basic " + btoa("ota:" + pass); }

        $('#ota_set').click(function(){
            act("otapass", {pass: $('#ota_new').val()}).done(function(){
                $('#ota_new').val("");
                loadConfig();
            });
        });
        $('#ota_change').click(function(){
            act("otapass", {old: $('#ota_old').val(),
                            pass: $('#ota_new2').val()}).done(function(){
                $('#ota_old').val(""); $('#ota_new2').val("");
                alert("OTA password changed.");
            });
        });
        $('#ota_upload').click(function(){
            var f = $('#ota_file')[0].files[0];
            if(!f){ alert("Choose a firmware .bin first."); return; }
            var pass = $('#ota_pass').val();
            if(!pass){ alert("Enter the OTA password."); return; }
            if(!confirm("Replace the firmware with "+f.name+" ("
                        +Math.round(f.size/1024)+" KB)?\n\n"
                        +"The oven reboots on success."))
                return;
            var fd = new FormData();
            fd.append("firmware", f, f.name);
            // Same-origin POST to port 80, unlike every other mutation here:
            // a cross-origin upload carrying an Authorization header needs
            // CORS credentials, and there is no reason to invite that.
            var xhr = new XMLHttpRequest();
            xhr.open("POST", "/update", true);
            xhr.setRequestHeader("Authorization", otaAuth(pass));
            xhr.upload.onprogress = function(e){
                if(e.lengthComputable)
                    $('#ota_stat').text(Math.round(100*e.loaded/e.total)+"%");
            };
            xhr.onload = function(){
                $('#ota_stat').text(xhr.responseText || ("HTTP "+xhr.status));
            };
            // The board reboots the moment it has answered, so a dropped
            // connection after a 200 is success, not failure.
            xhr.onerror = function(){ $('#ota_stat').text("Upload failed"); };
            $('#ota_stat').text("uploading\u2026");
            xhr.send(fd);
        });

        $('#toggle').click(function(){ $('#panel').toggle(); });

        $('#action').click(function () {
            $.ajax({
                url : "http://"+window.location.hostname+":8080/"+(running?"stop":"start")+"?t="+Date.now(),
                dataType: "text",
                success : function (data) {
                  console.log("sucess",data);
                    if(data!="OK")
                        alert("Controller not ready!");
                },
                error:function(a,b,c){
                  console.log("error",a,b,c);
                    alert("Communication error!");
                }
            });
        });
        $('#export').click(function () {
            var csvFormattedDataTable = google.visualization.dataTableToCsv(chartdata).replace(/[,]/g,";");
            var encodedUri = 'data:application/csv;charset=utf-8,' + "Time;State;Temperature;Setpoint;Power\n" +encodeURIComponent(csvFormattedDataTable);
            this.href = encodedUri;
            this.download = 'Reflow_'+(new Date().toISOString().substring(0, 16).replace(/[\-:T]/g,"_"))+'.csv';
            this.target = '_blank';
        });        
          
      });
      
    </script>
  </head>
  <body>
    <div id="readout">
      <div class="tile" id="r_state">
        <div class="label">State</div>
        <div class="value" id="v_state">&mdash;</div>
        <div class="sub" id="v_step">&nbsp;</div>
      </div>
      <div class="tile" id="r_temp">
        <div class="label">Temperature</div>
        <div class="value"><span id="v_temp">&mdash;</span><span class="unit">&deg;C</span></div>
        <div class="sub" id="v_rate">&nbsp;</div>
      </div>
      <div class="tile" id="r_target">
        <div class="label">Target</div>
        <div class="value"><span id="v_target">&mdash;</span><span class="unit">&deg;C</span></div>
        <div class="sub" id="v_corridor">&nbsp;</div>
      </div>
      <div class="tile off" id="r_heat">
        <div class="label">Heater</div>
        <div class="value" id="v_heat">&mdash;</div>
        <div class="sub" id="v_duty">&nbsp;</div>
      </div>
      <div class="tile" id="r_mcu">
        <div class="label">Electronics</div>
        <div class="value"><span id="v_mcu">&mdash;</span><span class="unit">&deg;C</span></div>
        <div class="sub" id="v_mcu_note">&nbsp;</div>
      </div>
    </div>
    <div id="door" style="display:none; flex:none; background:#fd7;
         border-top:2px solid #a70; border-bottom:2px solid #a70;
         padding:8px 14px; font-weight:bold; font-size:20px">
      Peak reached &mdash; OPEN THE OVEN DOOR NOW
      <button id="door_mute" style="margin-left:14px; font-size:15px">Silence</button>
    </div>
    <div id="stage">
    <div id="controls">        
        Action: &nbsp;&nbsp;&nbsp;<button id="action" ></button>
        <button id="toggle">Settings</button> <br>
        <a id="export" href="#">Download Graph</a>
        <div id="fault" style="display:none; color:#fff; background:#c00; padding:4px; margin-top:4px; font-weight:bold"></div>
        <div id="warn" style="display:none; color:#000; background:#fd7; border:1px solid #a70; padding:4px; margin-top:4px"></div>

        <div id="panel" style="display:none; margin-top:8px; font-size:12px">
          <fieldset><legend>Profile slot</legend>
            <select id="slot"></select>
            <button id="load">Load</button>
            <button id="save">Save to slot</button>
          </fieldset>

          <fieldset><legend>Profile</legend>
            <label>Name <input id="p_name" maxlength="10" size="10"></label><br>
            <label>Steps &mdash; one per line, max <span id="p_limit">?</span>:<br>
              <code>target&deg;C, min s, max s</code> &nbsp;or&nbsp;
              <code>target&deg;C, min &deg;C/s, max &deg;C/s, r</code><br>
              <textarea id="p_steps" rows="7" cols="34"></textarea></label><br>
            <span>Rate-bounded steps shorten when the chamber starts warm
              instead of flattening &mdash; use them for a ramp from ambient.
              Duration-bounded steps hold their time, which is what a soak or a
              peak needs.</span><br>
            <button id="p_apply">Apply</button>
            <span>(not stored until saved to a slot)</span>
          </fieldset>

          <fieldset><legend>Oven (thermal inertia)</legend>
            <label>Thermal lag (s) <input id="o_thermalLag" type="number" step="0.1" size="5"></label>
            <label>Measure at (&deg;C) <input id="o_measureTemp" type="number" size="5"></label>
            <button id="o_apply">Apply</button>
            <button id="o_measure">Measure</button><br>
            <span id="o_measured"></span><br>
            <label>Probe calibration &mdash; true = c2&middot;raw&sup2; + c1&middot;raw + c0<br>
              <input id="o_calC2" size="14"> <input id="o_calC1" size="12">
              <input id="o_calC0" size="10">
              <button id="cal_apply">Apply</button></label><br>
            <span>A property of the thermocouple, AD595 and ADC &mdash; not of
              the oven, so a different chamber does not invalidate it but a
              different probe or controller board does. Enter
              <code>0 1 0</code> for no correction. Refused unless the curve
              rises across 0-350&deg;C and stays within 30&deg;C of the raw
              reading: a calibration that reads low runs the oven hot without
              telling you.</span><br>
            <span>How far the oven keeps climbing after the relay opens. Measure
              it loaded with a representative board &mdash; an empty oven is a
              different oven.</span>
          </fieldset>

          <fieldset><legend>Hold temperature</legend>
            <label>Hold at (&deg;C) <input id="hold_temp" type="number" value="100" size="5"></label>
            <button id="hold_set">Hold</button>
            <button id="hold_stop">Stop</button><br>
            <span>Regulates to a fixed temperature so the oven probe can be
              compared against a reference meter. Send a new value to step to
              the next level without stopping &mdash; the chamber does not have
              to cool down in between. Read both instruments at the same
              moment: power is quantised, so the oven settles into a slow cycle
              about the setpoint rather than sitting perfectly still, and
              reading them together cancels it.</span>
          </fieldset>

          <fieldset><legend>Manual heating</legend>
            <label>Power (%) <input id="manual_power" type="number" value="0" size="4"></label>
            <button id="manual_set">Set</button>
          </fieldset>

          <fieldset><legend>Firmware (OTA)</legend>
            <div id="fw_ver" style="margin-bottom:6px; font-weight:bold">&mdash;</div>
            <div id="ota_none">
              <label>Set OTA password <input id="ota_new" type="password" size="14"></label>
              <button id="ota_set">Set</button><br>
              <span>OTA is refused until a password is set. Set it once, on a
                network you trust &mdash; the first set cannot be
                authenticated.</span>
            </div>
            <div id="ota_have" style="display:none">
              <label>Password <input id="ota_pass" type="password" size="14"></label>
              <input id="ota_file" type="file" accept=".bin">
              <button id="ota_upload">Upload firmware</button>
              <span id="ota_stat"></span><br>
              <label>Change: current <input id="ota_old" type="password" size="10"></label>
              <label>new <input id="ota_new2" type="password" size="10"></label>
              <button id="ota_change">Change</button><br>
              <span>Refused while a cycle is running or the oven is heating
                manually. The oven reboots into the new firmware on
                success.</span>
            </div>
          </fieldset>

          <fieldset><legend>Danger</legend>
            <button id="reset">Factory reset</button>
          </fieldset>
        </div>
    </div>
    <div id="chart_div"></div>
    </div>
  </body>

</html>
)=====";