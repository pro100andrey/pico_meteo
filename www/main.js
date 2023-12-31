
const SENSOR_UPDATE_MS = 2000;

const temperatureElem = document.getElementById("temperatureValue");
const pressureElem = document.getElementById("pressureValue");
const humidityElem = document.getElementById("humidityValue");
const errorMessageElem = document.getElementById("errorMessageValue");

/* Common function for fetching the response for a URL path. */
async function getResponse(url) {
    let response = await fetch(url);
    if (!response.ok) {
        throw new Error(url + " response status: " + response.status);
    }

    return response;
}

async function updateSensorData() {
    let data = null;

    try {
        let response = await getResponse("/sensor");
        data = await response.json();
    }
    catch (ex) {
        toast(ex);
        return;
    }

    temperatureElem.textContent = data.temperature;
    pressureElem.textContent = data.pressure;
    humidityElem.textContent = data.humidity;
}

async function updateOnVisible() {
    if (document.visibilityState === "hidden") {
        return;
    }

    await updateSensorData();
}

async function init() {
    await updateSensorData();

    document.addEventListener("visibilitychange", updateOnVisible);
    setInterval(updateSensorData, SENSOR_UPDATE_MS)
}

/* Run initialization when the document has been loaded. */
if (document.readyState !== "loading") {
    init();
} else {
    document.addEventListener("DOMContentLoaded", init);
}
