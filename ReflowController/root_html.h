const char ROOT_HTML[] PROGMEM = R"=====(
<html>
  <head>
    <meta charset="UTF-8">
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

             if(data.fault){
                 $('#fault').text("FAULT: "+data.fault+" -- power cycle the oven").show();
             }

             if(data.state=="Ready")
             {
                 //clear all
                 initchartdata();
                 lastState="Ready";
                 $('#action').text("Start Reflow");
                 running=false;
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
                 chartdata.addRow([data.time/1000, lable, data.temp, data.setpoint, data.power]);
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
           .always(function() {
             setTimeout(loadstatus, Math.max(10,1000-(Date.now()-lastcall)));
           });

        }

        loadstatus();

        // Everything below replaces the rotary-encoder menu: profile slots,
        // profile fields, PID gains, autotune parameters and manual heating.
        var PROFILE_FIELDS = ['name','rampUpRate','soakTemp','soakDuration',
                              'peakTemp','peakDuration','rampDownRate'];
        var PID_FIELDS     = ['kp','ki','kd'];
        var TUNING_FIELDS  = ['output','noiseBand','step','lookback'];

        function act(path, params){
            return $.ajax({
                url: "http://"+window.location.hostname+":8080/"+path,
                data: $.extend({t: Date.now()}, params || {}),
                dataType: "text"
            }).fail(function(x){
                alert(x.status==409 ? "Controller busy -- stop the cycle first."
                                    : "Communication error!");
            });
        }

        function loadConfig(){
            $.getJSON("config?t="+Date.now()).done(function(c){
                PROFILE_FIELDS.forEach(function(f){ $('#p_'+f).val(c.profile[f]); });
                PID_FIELDS.forEach(function(f){ $('#pid_'+f).val(c.pid[f]); });
                TUNING_FIELDS.forEach(function(f){ $('#tune_'+f).val(c.tuning[f]); });
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

        function collect(prefix, fields){
            var out = {};
            fields.forEach(function(f){ out[f] = $('#'+prefix+f).val(); });
            return out;
        }

        $('#p_apply').click(function(){
            act("profile/edit", collect('p_', PROFILE_FIELDS)).done(loadConfig);
        });
        $('#pid_apply').click(function(){
            act("pid", collect('pid_', PID_FIELDS)).done(loadConfig);
        });
        $('#tune_apply').click(function(){
            act("tuning", collect('tune_', TUNING_FIELDS)).done(loadConfig);
        });
        $('#load').click(function(){
            act("profile/load", {id: $('#slot').val()}).done(loadConfig);
        });
        $('#save').click(function(){
            act("profile/save", {id: $('#slot').val()}).done(loadConfig);
        });
        $('#autotune').click(function(){
            if(confirm("Run autotune? This heats the oven for a long time."))
                act("tune");
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
    <div style="position: absolute; z-index: 1000; background: rgba(255,255,255,0.9); padding: 6px">        
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
            <label>Ramp up (&deg;C/s) <input id="p_rampUpRate" type="number" step="0.1" size="5"></label><br>
            <label>Soak temp (&deg;C) <input id="p_soakTemp" type="number" size="5"></label><br>
            <label>Soak time (s) <input id="p_soakDuration" type="number" size="5"></label><br>
            <label>Peak temp (&deg;C) <input id="p_peakTemp" type="number" size="5"></label><br>
            <label>Peak time (s) <input id="p_peakDuration" type="number" size="5"></label><br>
            <label>Ramp down (&deg;C/s) <input id="p_rampDownRate" type="number" step="0.1" size="5"></label><br>
            <button id="p_apply">Apply</button>
            <span>(not stored until saved to a slot)</span>
          </fieldset>

          <fieldset><legend>PID</legend>
            <label>Kp <input id="pid_kp" type="number" step="0.01" size="5"></label>
            <label>Ki <input id="pid_ki" type="number" step="0.01" size="5"></label>
            <label>Kd <input id="pid_kd" type="number" step="0.01" size="5"></label>
            <button id="pid_apply">Apply</button>
          </fieldset>

          <fieldset><legend>Autotune</legend>
            <label>Output (%) <input id="tune_output" type="number" size="4"></label>
            <label>Noise band <input id="tune_noiseBand" type="number" size="4"></label>
            <label>Step (%) <input id="tune_step" type="number" size="4"></label>
            <label>Lookback (s) <input id="tune_lookback" type="number" size="4"></label>
            <button id="tune_apply">Apply</button>
            <button id="autotune">Run autotune</button>
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
    <div id="chart_div" style="width: 100%; height: 100%;"></div>
  </body>

</html>
)=====";