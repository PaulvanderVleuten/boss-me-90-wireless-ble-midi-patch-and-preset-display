#ifndef INDEX_H
#define INDEX_H

const char HTML_CONTENT[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>ME-90 Patch Display</title>
<style>
  body { 
    font-family: Arial, sans-serif; 
    margin: 20px; 
    background: #222; 
    color: #fff; 
  }
  #patch-list { 
    max-height: 600px; 
    overflow-y: auto; 
    background: #333; 
    padding: 15px; 
    border-radius: 8px;
    display: grid;
    grid-template-columns: repeat(4, 1fr);
    gap: 10px 15px;
  }
  .patch-row { 
    display: flex; 
    align-items: center; 
    justify-content: space-between;
    background: #2b2b2b;
    padding: 6px;
    border-radius: 4px;
  }
  .patch-label { 
    font-weight: bold; 
    width: 55px; 
    color: #00ffcc; 
    font-size: 13px;
  }
  input[type="text"] { 
    background: #444; 
    color: #fff; 
    border: 1px solid #555; 
    padding: 4px 6px; 
    border-radius: 4px; 
    flex-grow: 1;
    min-width: 50px;
    margin-right: 6px; 
    font-size: 13px;
    transition: background 0.2s ease, color 0.2s ease;
  }
  input[type="color"].hidden-picker {
    position: absolute;
    width: 0;
    height: 0;
    opacity: 0;
    padding: 0;
    margin: 0;
    border: none;
    pointer-events: none;
  }
  button { 
    background: #ffaa00; 
    border: none; 
    padding: 5px 8px; 
    border-radius: 4px; 
    cursor: pointer; 
    font-weight: bold; 
    font-size: 12px;
  }
  button:hover { 
    background: #e09600; 
  }

  @media (max-width: 1024px) {
    #patch-list { grid-template-columns: repeat(2, 1fr); }
  }
  @media (max-width: 480px) {
    #patch-list { grid-template-columns: 1fr; }
  }
</style>
</head>
<body>
  <h1>ME-90 Patch Manager</h1>
  <div id="patch-manager">
    <h2>Preset Name Editor
    <span style="font-size: 0.5em; font-weight: normal;"> Right-click or Long Press for Preset Background color</span></h2>
    <div id="patch-list"></div>
  </div>
<script>
  var ws = new WebSocket('ws://' + window.location.hostname + ':81/');
  
  ws.onopen = function() { console.log('WebSocket Connection Opened!'); };
  ws.onclose = function() { console.log('WebSocket Connection Closed!'); };
  
  // Helper function to calculate light vs dark backgrounds
  function getContrastColor(hexColor) {
    // Remove the '#' character if present
    const hex = hexColor.replace('#', '');
    
    // Convert 3-character hex to 6-character hex
    const r = parseInt(hex.length === 3 ? hex[0]+hex[0] : hex.substring(0, 2), 16);
    const g = parseInt(hex.length === 3 ? hex[1]+hex[1] : hex.substring(2, 4), 16);
    const b = parseInt(hex.length === 3 ? hex[2]+hex[2] : hex.substring(4, 6), 16);
    
    // Calculate relative luminance using standard W3C formula
    const yiq = ((r * 299) + (g * 587) + (b * 114)) / 1000;
    
    // If the brightness is high, use black text; otherwise use white text
    return (yiq >= 128) ? '#000000' : '#ffffff';
  }

  ws.onmessage = function(event) {
    let data = event.data;
    if (data.startsWith('INIT_NAMES:')) {
      let patches = data.replace('INIT_NAMES:', '').split(',');
      
      for (let i = 0; i < patches.length; i++) {
        let inputField = document.getElementById(`input-patch-${i}`);
        let colorField = document.getElementById(`color-patch-${i}`);
        
        if (patches[i].includes('|')) {
          let parts = patches[i].split('|');
          let savedName = parts[0];
          let savedColor = parts[1];

          if (inputField) {
            inputField.value = savedName;
            inputField.style.backgroundColor = savedColor;
            inputField.style.color = getContrastColor(savedColor); // Apply readable font color
          }
          if (colorField) {
            colorField.value = savedColor;
          }
        } else {
          if (inputField) inputField.value = patches[i];
        }
      }
    } else {
      console.log('Server Message: ' + data);
    }
  };

  const patchListDiv = document.getElementById('patch-list');
  for (let i = 0; i < 72; i++) {
    let prefix = (i < 36) ? 'U' : 'P';
    let bank = Math.floor((i % 36) / 4) + 1;
    let patch = (i % 4) + 1;
    let row = document.createElement('div');
    row.className = 'patch-row';
    
    row.innerHTML = `<span class="patch-label">${prefix}${bank}-${patch}:</span>` +
                    `<input type="text" id="input-patch-${i}" placeholder="Empty Patch" oncontextmenu="openColorPicker(event, ${i})">` +
                    `<input type="color" id="color-patch-${i}" class="hidden-picker" value="#ffffff" oninput="updateInputColor(${i})">` +
                    `<button onclick="sendPatchUpdate(${i})">Save</button>`;
    patchListDiv.appendChild(row);
  }

  function openColorPicker(event, index) {
    event.preventDefault(); 
    const colorPicker = document.getElementById(`color-patch-${index}`);
    if (colorPicker) colorPicker.click();
  }

  function updateInputColor(index) {
    const colorPicker = document.getElementById(`color-patch-${index}`);
    const textInput = document.getElementById(`input-patch-${index}`);
    if (colorPicker && textInput) {
      const selectedColor = colorPicker.value;
      textInput.style.backgroundColor = selectedColor;
      textInput.style.color = getContrastColor(selectedColor); // Dynamically update readable font color
    }
  }

  function sendPatchUpdate(index) {
    const nameInput = document.getElementById(`input-patch-${index}`).value || "Empty Patch";
    const colorInput = document.getElementById(`color-patch-${index}`).value;
    
    const message = `UPDATE_PATCH|${index}|${nameInput}|${colorInput}`;
    ws.send(message);
  }
</script>
</body>
</html>
)rawliteral";

#endif
