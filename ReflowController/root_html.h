const char ROOT_HTML[] PROGMEM = R"=====(
<html>
  <head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
      html, body { height: 100%; }
      body { margin: 0; display: flex; flex-direction: column;
             font-family: system-ui, -apple-system, "Segoe UI", Roboto, sans-serif; }

      /* Live readout. Always visible, above the graph, legible across a
         workshop -- this is the thing you look at while the oven runs. */
      #readout { flex: none; display: flex; flex-wrap: wrap; align-items: stretch;
                 gap: 1px; background: #d4d4d8; border-bottom: 1px solid #d4d4d8; }
      #readout .tile { flex: 1 1 140px; background: #fafafa; padding: 8px 14px; }
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
      #r_heat .value { font-size: 34px; letter-spacing: .04em; }
      #r_heat.on  { background: #fef2f2; }
      #r_heat.on  .value { color: #dc2626; }
      #r_heat.off .value { color: #a1a1aa; }

      /* No fresh /status: the numbers on screen are history, so say so. */
      #readout.stale { opacity: .45; }
      #readout.stale .value::after { content: " ?"; color: #a1a1aa; }

      #stage { position: relative; flex: 1 1 auto; min-height: 0; }
      #chart_div { width: 100%; height: 100%; }
      #controls { position: absolute; top: 0; left: 0; z-index: 1000;
                  background: rgba(255,255,255,0.9); padding: 6px; }
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
            chartdata.addColumn('number', "Temperatur");
            chartdata.addColumn('number', "Setpoint");
            chartdata.addColumn('number', "Corridor low");
            chartdata.addColumn('number', "Corridor high");
            chartdata.addColumn('number', "Power");
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
              title: 'Temps (°C)',
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
             $('#v_state').text(data.state);
             $('#v_step').html(data.steps > 0 && data.state!="Ready"
                 && data.state!="Complete"
                 ? "step "+(data.step+1)+" of "+data.steps
                 : "&nbsp;");
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
             // -------------------------------------------------------

             if(data.openDoor){
                 $('#door').show();
             } else {
                 $('#door').hide();
             }

             if(data.fault){
                 $('#fault').text("FAULT: "+data.fault+" -- power cycle the oven").show();
             }

             if(data.state=="Ready")
             {
                 //clear all
                 initchartdata();
                 lastState="Ready";
                 running=false;
                 $('#action').text(
                     (startMax!==null && data.temp > startMax)
                     ? "Too hot ("+Math.round(data.temp)+"\u00B0C)"
                     : "Start Reflow");
             }
             else{
                 if(data.state=="Complete"){
                    $('#action').text("Reset");
                 }
                 else{
                    $('#action').text("Cancel");
                 }
                 running=true;
             }

             var lable=null;
             if(data.state!=lastState)
             {
                lastState= data.state;
                lable=data.state;
             }
             

             if(data.state!="Ready" && data.state!="Complete"){
                 chartdata.addRow([data.time/1000, lable, data.temp, data.setpoint,
                                   data.low, data.high, data.power]);
                 classicOptions.hAxis.viewWindow.max=Math.max(100,Math.round(data.time/1000.0)+10);
             }
             if(data.state=="Ready")
             {
                 classicOptions.hAxis.viewWindow.max=100;
             }
             if(data.state=="Complete")
             {
                 classicOptions.hAxis.viewWindow.max=null;
             }
             

             classicChart.draw(chartdata, classicOptions);
           })
           .fail(function() {
             $('#readout').addClass('stale');
           })
           .always(function() {
             setTimeout(loadstatus, Math.max(10,1000-(Date.now()-lastcall)));
           });

        }

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
                startMax = c.oven.startMaxTemp;
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
        $('#reset').click(function(){
            if(confirm("Factory reset? This erases all profiles and the WiFi credentials."))
                act("factoryreset").done(loadConfig);
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
            var encodedUri = 'data:application/csv;charset=utf-8,' + "Time;State;Temperatur;Setpoint;Power\n" +encodeURIComponent(csvFormattedDataTable);
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
        <div class="value" style="font-size:34px" id="v_state">&mdash;</div>
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
    </div>
    <div id="door" style="display:none; flex:none; background:#fd7;
         border-top:2px solid #a70; border-bottom:2px solid #a70;
         padding:8px 14px; font-weight:bold; font-size:20px">
      Peak reached &mdash; OPEN THE OVEN DOOR NOW
    </div>
    <div id="stage">
    <div id="controls">        
        Action: &nbsp;&nbsp;&nbsp;<button id="action" ></button>
        <button id="toggle">Settings</button> <br>
        <a id="export" href="#">Download Graph</a>
        <div id="fault" style="display:none; color:#fff; background:#c00; padding:4px; margin-top:4px; font-weight:bold"></div>

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
            <span>How far the oven keeps climbing after the relay opens. Measure
              it loaded with a representative board &mdash; an empty oven is a
              different oven.</span>
          </fieldset>

          <fieldset><legend>Manual heating</legend>
            <label>Power (%) <input id="manual_power" type="number" value="0" size="4"></label>
            <button id="manual_set">Set</button>
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